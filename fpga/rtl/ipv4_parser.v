// ipv4_parser.v - IPv4 layer (step 3.3)

module ipv4_parser #(
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

    localparam IP_PROTO_UDP = 8'd17;

    localparam S_SNIFF = 1'b0,
               S_EMIT  = 1'b1;

    reg               state;
    reg [3:0]         nheld;                    // beats held (0..8, up to IHL=60)
    reg [3:0]         drain_idx;
    reg [15:0]        skip_r, keep_r;
    reg               drop_r;

    // per-frame header facts, latched on beat 0 / accumulated over the header
    reg [15:0]        ihl_r, total_r;
    reg               bad_r;                    // version/ihl/proto reject
    reg [31:0]        csum_r;                   // running one's-complement sum

    // header can span up to 8 beats (IHL 60)
    reg [DATA_W-1:0]   hb_data [0:7];
    reg [DATA_W/8-1:0] hb_keep [0:7];
    reg                hb_last [0:7];

    // gearbox interface
    reg  [DATA_W-1:0]   g_tdata;
    reg  [DATA_W/8-1:0] g_tkeep;
    reg                 g_tvalid;
    wire                g_tready;
    reg                 g_tlast;

    axis_skip_align #(.DATA_W(DATA_W)) u_gear (
        .clk(clk), .rst(rst),
        .skip(skip_r), .keep_len(keep_r), .drop(drop_r),
        .s_axis_tdata(g_tdata), .s_axis_tkeep(g_tkeep),
        .s_axis_tvalid(g_tvalid), .s_axis_tready(g_tready), .s_axis_tlast(g_tlast),
        .m_axis_tdata(m_axis_tdata), .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid), .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast)
    );

    wire draining = (drain_idx < nheld);

    // combinational: stream muxing
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

    // header bytes of the beat being accepted, as big-endian 16-bit words
    wire [16:0] w0 = {s_axis_tdata[8*0 +: 8], s_axis_tdata[8*1 +: 8]};
    wire [16:0] w1 = {s_axis_tdata[8*2 +: 8], s_axis_tdata[8*3 +: 8]};
    wire [16:0] w2 = {s_axis_tdata[8*4 +: 8], s_axis_tdata[8*5 +: 8]};
    wire [16:0] w3 = {s_axis_tdata[8*6 +: 8], s_axis_tdata[8*7 +: 8]};

    wire [3:0] ver_c       = s_axis_tdata[7:4];      // beat 0 byte 0 hi nibble
    wire [3:0] ihl_words_c = s_axis_tdata[3:0];      // beat 0 byte 0 lo nibble
    wire [15:0] ihl_c      = (nheld == 4'd0) ? {10'd0, ihl_words_c, 2'd0} : ihl_r;
    wire [15:0] total_c    = {s_axis_tdata[8*2 +: 8], s_axis_tdata[8*3 +: 8]};
    wire [7:0]  proto_c    = s_axis_tdata[8*1 +: 8];  // beat 1 byte 9 (lane 1)

    wire [15:0] start_c    = {8'd0, nheld, 3'd0};     // 8 * nheld
    wire [15:0] rem_c      = ihl_c - start_c;          // header bytes left at beat start
    wire [31:0] wsum_full  = w0 + w1 + w2 + w3;
    wire [31:0] wsum_half  = w0 + w1;
    wire [31:0] contrib_c  = (rem_c >= 16'd8) ? wsum_full :
                             (rem_c == 16'd4) ? wsum_half : 32'd0;
    wire [31:0] csum_next  = (nheld == 4'd0) ? wsum_full : (csum_r + contrib_c);
    wire        hdr_done_c = (rem_c <= 16'd8);          // this is the last header beat

    // fold the running sum to 16 bits; a valid header folds to 0xFFFF
    wire [16:0] fold1 = csum_next[15:0] + csum_next[31:16];
    wire [16:0] fold2 = fold1[15:0]     + {16'd0, fold1[16]};
    wire [16:0] fold3 = fold2[15:0]     + {16'd0, fold2[16]};
    wire        csum_ok = (fold3[15:0] == 16'hFFFF);

    // payload length = total_length - header
    wire [15:0] keep_c = (total_r > ihl_r) ? (total_r - ihl_r) : 16'd0;

    always @(posedge clk) begin
        if (rst) begin
            state     <= S_SNIFF;
            nheld     <= 4'd0;
            drain_idx <= 4'd0;
            skip_r    <= 16'd0;
            keep_r    <= 16'd0;
            drop_r    <= 1'b0;
            ihl_r     <= 16'd0;
            total_r   <= 16'd0;
            bad_r     <= 1'b0;
            csum_r    <= 32'd0;
        end else case (state)

            S_SNIFF: if (s_axis_tvalid && s_axis_tready) begin
                hb_data[nheld] <= s_axis_tdata;
                hb_keep[nheld] <= s_axis_tkeep;
                hb_last[nheld] <= s_axis_tlast;
                nheld          <= nheld + 4'd1;
                csum_r         <= csum_next;

                if (nheld == 4'd0) begin              // beat 0: byte 0..7
                    ihl_r   <= ihl_c;
                    total_r <= total_c;
                    bad_r   <= (ver_c != 4'd4) || (ihl_words_c < 4'd5);
                end else if (nheld == 4'd1) begin     // beat 1: byte 9 = protocol
                    bad_r   <= bad_r | (proto_c != IP_PROTO_UDP);
                end

                if (s_axis_tlast) begin               // runt: ended in the header
                    skip_r <= 16'd20; keep_r <= 16'd0; drop_r <= 1'b1;
                    state  <= S_EMIT; drain_idx <= 4'd0;
                end else if (hdr_done_c) begin         // whole header now summed
                    skip_r <= ihl_c;
                    keep_r <= keep_c;
                    drop_r <= bad_r | ~csum_ok | (total_r < ihl_c);
                    state  <= S_EMIT; drain_idx <= 4'd0;
                end
            end

            S_EMIT: begin
                if (draining) begin
                    if (g_tready) begin
                        if (hb_last[drain_idx]) begin
                            state <= S_SNIFF; nheld <= 4'd0; drain_idx <= 4'd0;
                        end else begin
                            drain_idx <= drain_idx + 4'd1;
                        end
                    end
                end else if (s_axis_tvalid && g_tready && s_axis_tlast) begin
                    state <= S_SNIFF; nheld <= 4'd0; drain_idx <= 4'd0;
                end
            end
        endcase
    end

`ifndef SYNTHESIS
    initial begin
        $dumpfile("ipv4_parser.vcd");
        $dumpvars(0, ipv4_parser);
    end
`endif

endmodule
