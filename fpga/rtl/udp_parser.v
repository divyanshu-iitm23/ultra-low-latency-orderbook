// udp_parser.v - UDP layer (step 3.4)

module udp_parser #(
    parameter DATA_W    = 64,
    parameter DEST_PORT = 16'd26477         // frames.FEED_PORT
) (
    input  wire                clk,
    input  wire                rst,          // synchronous, active-high

    input  wire [DATA_W-1:0]   s_axis_tdata,
    input  wire [DATA_W/8-1:0] s_axis_tkeep,
    input  wire                s_axis_tvalid,
    output wire                s_axis_tready,
    input  wire                s_axis_tlast,

    output wire [DATA_W-1:0]   m_axis_tdata,
    output wire [DATA_W/8-1:0] m_axis_tkeep,
    output wire                m_axis_tvalid,
    input  wire                m_axis_tready,
    output wire                m_axis_tlast
);

    // beat-0 header fields (big-endian)
    wire [15:0] dport_c = {s_axis_tdata[8*2 +: 8], s_axis_tdata[8*3 +: 8]};
    wire [15:0] ulen_c  = {s_axis_tdata[8*4 +: 8], s_axis_tdata[8*5 +: 8]};

    // A segment that ends inside beat 0 without all 8 header bytes is a runt.
    wire runt_c = s_axis_tlast && !s_axis_tkeep[DATA_W/8-1];

    // length <= 8 means no payload: emit nothing. Dropping it also keeps us
    // clear of the gearbox's keep_len==0 sentinel ("to end of frame").
    wire drop_c = (dport_c != DEST_PORT) || (ulen_c <= 16'd8) || runt_c;

    wire [15:0] keep_c = (ulen_c > 16'd8) ? (ulen_c - 16'd8) : 16'd0;

    axis_skip_align #(.DATA_W(DATA_W)) u_gear (
        .clk(clk), .rst(rst),
        .skip(16'd8), .keep_len(keep_c), .drop(drop_c),
        .s_axis_tdata(s_axis_tdata), .s_axis_tkeep(s_axis_tkeep),
        .s_axis_tvalid(s_axis_tvalid), .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .m_axis_tdata(m_axis_tdata), .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid), .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast)
    );

`ifndef SYNTHESIS
    initial begin
        $dumpfile("udp_parser.vcd");
        $dumpvars(0, udp_parser);
    end
`endif

endmodule
