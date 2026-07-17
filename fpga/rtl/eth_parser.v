// eth_parser.v - Ethernet II framing layer (step 3.2)

module eth_parser #(
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
    output wire                m_axis_tlast
);

    localparam ETHERTYPE_IPV4 = 16'h0800;
    localparam ETHERTYPE_VLAN = 16'h8100;

    localparam S_SNIFF = 1'b0,                  // holding opening beats, deciding
               S_EMIT  = 1'b1;                  // feeding the gearbox

    reg               state;
    reg [1:0]         nheld;                    // beats held (0..3)
    reg [1:0]         drain_idx;                // next held beat to release
    reg [15:0]        skip_r;
    reg               drop_r;

    // up to 3 opening beats parked while we sniff the header
    reg [DATA_W-1:0]   hb_data [0:2];
    reg [DATA_W/8-1:0] hb_keep [0:2];
    reg                hb_last [0:2];

    // gearbox interface (this module drives the gearbox's slave port)
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

    // combinational
    always @(*) begin
        g_tvalid      = 1'b0;
        g_tdata       = {DATA_W{1'b0}};
        g_tkeep       = {(DATA_W/8){1'b0}};
        g_tlast       = 1'b0;
        s_axis_tready = 1'b0;

        if (state == S_SNIFF) begin
            s_axis_tready = 1'b1;               // slurp opening beats (<=3, room guaranteed)
        end else if (draining) begin
            g_tvalid      = 1'b1;               // replay a held beat
            g_tdata       = hb_data[drain_idx];
            g_tkeep       = hb_keep[drain_idx];
            g_tlast       = hb_last[drain_idx];
            s_axis_tready = 1'b0;               // input stalled while draining
        end else begin
            g_tvalid      = s_axis_tvalid;      // live pass-through, gearbox does the strip
            g_tdata       = s_axis_tdata;
            g_tkeep       = s_axis_tkeep;
            g_tlast       = s_axis_tlast;
            s_axis_tready = g_tready;
        end
    end

    // ethertype candidates from the beat currently being accepted
    wire [15:0] et_beat1 = {s_axis_tdata[8*4 +: 8], s_axis_tdata[8*5 +: 8]}; // bytes 12,13
    wire [15:0] et_beat2 = {s_axis_tdata[8*0 +: 8], s_axis_tdata[8*1 +: 8]}; // bytes 16,17

    integer i;
    always @(posedge clk) begin
        if (rst) begin
            state     <= S_SNIFF;
            nheld     <= 2'd0;
            drain_idx <= 2'd0;
            skip_r    <= 16'd0;
            drop_r    <= 1'b0;
        end else case (state)

            S_SNIFF: if (s_axis_tvalid && s_axis_tready) begin
                hb_data[nheld] <= s_axis_tdata;
                hb_keep[nheld] <= s_axis_tkeep;
                hb_last[nheld] <= s_axis_tlast;
                nheld          <= nheld + 2'd1;

                if (s_axis_tlast) begin                 // runt: frame ended in the header
                    skip_r <= 16'd14; drop_r <= 1'b1;   // drop; drain emits nothing
                    state  <= S_EMIT; drain_idx <= 2'd0;
                end else if (nheld == 2'd1) begin       // just took beat 1 (bytes 8-15)
                    if (et_beat1 != ETHERTYPE_VLAN) begin
                        skip_r <= 16'd14;
                        drop_r <= (et_beat1 != ETHERTYPE_IPV4);
                        state  <= S_EMIT; drain_idx <= 2'd0;
                    end
                    // else VLAN: keep sniffing, need beat 2 for the real ethertype
                end else if (nheld == 2'd2) begin       // just took beat 2 (bytes 16-23)
                    skip_r <= 16'd18;
                    drop_r <= (et_beat2 != ETHERTYPE_IPV4);
                    state  <= S_EMIT; drain_idx <= 2'd0;
                end
            end

            S_EMIT: begin
                if (draining) begin
                    if (g_tready) begin                 // held beat accepted by gearbox
                        if (hb_last[drain_idx]) begin    // whole frame fit in held beats
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

`ifndef SYNTHESIS
    initial begin
        $dumpfile("eth_parser.vcd");
        $dumpvars(0, eth_parser);
    end
`endif

endmodule
