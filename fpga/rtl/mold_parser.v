// mold_parser.v - MoldUDP64 layer (step 3.5)

module mold_parser #(
    parameter DATA_W = 64
) (
    input  wire                clk,
    input  wire                rst,             // synchronous, active-high

    input  wire [DATA_W-1:0]   s_axis_tdata,
    input  wire [DATA_W/8-1:0] s_axis_tkeep,
    input  wire                s_axis_tvalid,
    output reg                 s_axis_tready,
    input  wire                s_axis_tlast,

    output wire [DATA_W-1:0]   m_axis_tdata,
    output wire [DATA_W/8-1:0] m_axis_tkeep,
    output wire                m_axis_tvalid,
    input  wire                m_axis_tready,
    output wire                m_axis_tlast,

    output reg                 hdr_valid,       // 1-cycle strobe per decoded header
    output reg  [63:0]         pkt_seq,
    output reg  [15:0]         pkt_count,
    output reg                 seq_gap          // 1-cycle strobe: seq != expected
);

    localparam COUNT_HEARTBEAT = 16'h0000;
    localparam COUNT_END_SESS  = 16'hFFFF;

    localparam S_SNIFF = 1'b0,
               S_EMIT  = 1'b1;

    reg               state;
    reg [1:0]         nheld;                    // beats held (0..3)
    reg [1:0]         drain_idx;
    reg [15:0]        skip_r;
    reg               drop_r;

    reg [47:0]        seq_hi_r;                 // bytes 10-15, latched on beat 1

    // gap tracking -- the only state that survives across datagrams
    reg [63:0]        expected_seq;
    reg               have_expect;

    reg [DATA_W-1:0]   hb_data [0:2];
    reg [DATA_W/8-1:0] hb_keep [0:2];
    reg                hb_last [0:2];

    // gearbox interface
    reg  [DATA_W-1:0]   g_tdata;
    reg  [DATA_W/8-1:0] g_tkeep;
    reg                 g_tvalid;
    wire                g_tready;
    reg                 g_tlast;

    axis_skip_align #(.DATA_W(DATA_W)) u_gear (
        .clk(clk), .rst(rst),
        .skip(skip_r), .keep_len(16'd0), .drop(drop_r),
        .s_axis_tdata(g_tdata), .s_axis_tkeep(g_tkeep),
        .s_axis_tvalid(g_tvalid), .s_axis_tready(g_tready), .s_axis_tlast(g_tlast),
        .m_axis_tdata(m_axis_tdata), .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid), .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast)
    );

    wire draining = (drain_idx < nheld);

    // combinational: stream muxing (same pattern as eth/ipv4)
    always @(*) begin
        g_tvalid      = 1'b0;
        g_tdata       = {DATA_W{1'b0}};
        g_tkeep       = {(DATA_W/8){1'b0}};
        g_tlast       = 1'b0;
        s_axis_tready = 1'b0;

        if (state == S_SNIFF) begin
            s_axis_tready = 1'b1;
        end else if (draining) begin
            g_tvalid      = 1'b1;
            g_tdata       = hb_data[drain_idx];
            g_tkeep       = hb_keep[drain_idx];
            g_tlast       = hb_last[drain_idx];
            s_axis_tready = 1'b0;
        end else begin
            g_tvalid      = s_axis_tvalid;
            g_tdata       = s_axis_tdata;
            g_tkeep       = s_axis_tkeep;
            g_tlast       = s_axis_tlast;
            s_axis_tready = g_tready;
        end
    end

    // beat 1 carries sequence bytes 10-15 (lanes 2..7)
    wire [47:0] seq_hi_c = {s_axis_tdata[8*2 +: 8], s_axis_tdata[8*3 +: 8],
                            s_axis_tdata[8*4 +: 8], s_axis_tdata[8*5 +: 8],
                            s_axis_tdata[8*6 +: 8], s_axis_tdata[8*7 +: 8]};

    // beat 2 carries sequence bytes 16-17 (lanes 0,1) and count bytes 18-19 (lanes 2,3)
    wire [63:0] seq_c   = {seq_hi_r, s_axis_tdata[8*0 +: 8], s_axis_tdata[8*1 +: 8]};
    wire [15:0] count_c = {s_axis_tdata[8*2 +: 8], s_axis_tdata[8*3 +: 8]};

    // end-of-session is a marker, not 65535 messages: it advances nothing
    wire [15:0] eff_count_c = (count_c == COUNT_END_SESS) ? 16'd0 : count_c;
    wire        gap_c       = have_expect && (seq_c != expected_seq);
    wire        no_msgs_c   = (count_c == COUNT_HEARTBEAT) || (count_c == COUNT_END_SESS);

    // the header is complete only if byte 19 (beat 2, lane 3) actually arrived
    wire hdr_complete_c = s_axis_tkeep[3];

    always @(posedge clk) begin
        if (rst) begin
            state        <= S_SNIFF;
            nheld        <= 2'd0;
            drain_idx    <= 2'd0;
            skip_r       <= 16'd0;
            drop_r       <= 1'b0;
            seq_hi_r     <= 48'd0;
            expected_seq <= 64'd0;
            have_expect  <= 1'b0;
            hdr_valid    <= 1'b0;
            pkt_seq      <= 64'd0;
            pkt_count    <= 16'd0;
            seq_gap      <= 1'b0;
        end else begin
            hdr_valid <= 1'b0;                   // strobes default low
            seq_gap   <= 1'b0;

            case (state)
            S_SNIFF: if (s_axis_tvalid && s_axis_tready) begin
                hb_data[nheld] <= s_axis_tdata;
                hb_keep[nheld] <= s_axis_tkeep;
                hb_last[nheld] <= s_axis_tlast;
                nheld          <= nheld + 2'd1;

                if (nheld == 2'd1)
                    seq_hi_r <= seq_hi_c;         // stash the high 6 bytes of seq

                if (nheld == 2'd2) begin          // beat 2: seq completes, count arrives
                    if (!hdr_complete_c) begin    // runt: died inside the header
                        skip_r <= 16'd20; drop_r <= 1'b1;
                    end else begin
                        pkt_seq      <= seq_c;
                        pkt_count    <= count_c;
                        hdr_valid    <= 1'b1;
                        seq_gap      <= gap_c;
                        expected_seq <= seq_c + {48'd0, eff_count_c};
                        have_expect  <= 1'b1;

                        skip_r <= 16'd20;
                        drop_r <= no_msgs_c;      // heartbeat / end-of-session
                    end
                    state <= S_EMIT; drain_idx <= 2'd0;
                end else if (s_axis_tlast) begin  // ended before the header finished
                    skip_r <= 16'd20; drop_r <= 1'b1;
                    state  <= S_EMIT; drain_idx <= 2'd0;
                end
            end

            S_EMIT: begin
                if (draining) begin
                    if (g_tready) begin
                        if (hb_last[drain_idx]) begin
                            state <= S_SNIFF; nheld <= 2'd0; drain_idx <= 2'd0;
                        end else begin
                            drain_idx <= drain_idx + 2'd1;
                        end
                    end
                end else if (s_axis_tvalid && g_tready && s_axis_tlast) begin
                    state <= S_SNIFF; nheld <= 2'd0; drain_idx <= 2'd0;
                end
            end
            endcase
        end
    end

`ifndef SYNTHESIS
    initial begin
        $dumpfile("mold_parser.vcd");
        $dumpvars(0, mold_parser);
    end
`endif

endmodule
