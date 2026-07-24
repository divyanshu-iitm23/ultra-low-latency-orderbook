// feed_top.v - the full front-end (step 3.6)

module feed_top #(
    parameter DATA_W = 64
) (
    input  wire                clk,
    input  wire                rst,             // synchronous, active-high

    // slave: raw Ethernet frames
    input  wire [DATA_W-1:0]   s_axis_tdata,
    input  wire [DATA_W/8-1:0] s_axis_tkeep,
    input  wire                s_axis_tvalid,
    output wire                s_axis_tready,
    input  wire                s_axis_tlast,

    // decoded ITCH message bundle (straight from itch_parser)
    output wire                msg_valid,
    output wire                msg_modeled,
    output wire [7:0]          msg_type,
    output wire [47:0]         msg_timestamp,
    output wire [63:0]         msg_order_ref,
    output wire [63:0]         msg_new_order_ref,
    output wire [7:0]          msg_side,
    output wire [31:0]         msg_shares,
    output wire [31:0]         msg_price,
    output wire [63:0]         msg_stock,

    // MoldUDP64 side-band (from mold_parser)
    output wire                hdr_valid,
    output wire [63:0]         pkt_seq,
    output wire [15:0]         pkt_count,
    output wire                seq_gap
);

    // inter-layer AXI-Stream hops: e=eth->ipv4, i=ipv4->udp, u=udp->mold, m=mold->itch
    wire [DATA_W-1:0]   e_td, i_td, u_td, m_td;
    wire [DATA_W/8-1:0] e_tk, i_tk, u_tk, m_tk;
    wire                e_tv, i_tv, u_tv, m_tv;
    wire                e_tr, i_tr, u_tr, m_tr;   // ready flows upstream
    wire                e_tl, i_tl, u_tl, m_tl;

    eth_parser #(.DATA_W(DATA_W)) u_eth (
        .clk(clk), .rst(rst),
        .s_axis_tdata(s_axis_tdata), .s_axis_tkeep(s_axis_tkeep),
        .s_axis_tvalid(s_axis_tvalid), .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .m_axis_tdata(e_td), .m_axis_tkeep(e_tk), .m_axis_tvalid(e_tv),
        .m_axis_tready(e_tr), .m_axis_tlast(e_tl)
    );

    ipv4_parser #(.DATA_W(DATA_W)) u_ipv4 (
        .clk(clk), .rst(rst),
        .s_axis_tdata(e_td), .s_axis_tkeep(e_tk), .s_axis_tvalid(e_tv),
        .s_axis_tready(e_tr), .s_axis_tlast(e_tl),
        .m_axis_tdata(i_td), .m_axis_tkeep(i_tk), .m_axis_tvalid(i_tv),
        .m_axis_tready(i_tr), .m_axis_tlast(i_tl)
    );

    udp_parser #(.DATA_W(DATA_W)) u_udp (
        .clk(clk), .rst(rst),
        .s_axis_tdata(i_td), .s_axis_tkeep(i_tk), .s_axis_tvalid(i_tv),
        .s_axis_tready(i_tr), .s_axis_tlast(i_tl),
        .m_axis_tdata(u_td), .m_axis_tkeep(u_tk), .m_axis_tvalid(u_tv),
        .m_axis_tready(u_tr), .m_axis_tlast(u_tl)
    );

    mold_parser #(.DATA_W(DATA_W)) u_mold (
        .clk(clk), .rst(rst),
        .s_axis_tdata(u_td), .s_axis_tkeep(u_tk), .s_axis_tvalid(u_tv),
        .s_axis_tready(u_tr), .s_axis_tlast(u_tl),
        .m_axis_tdata(m_td), .m_axis_tkeep(m_tk), .m_axis_tvalid(m_tv),
        .m_axis_tready(m_tr), .m_axis_tlast(m_tl),
        .hdr_valid(hdr_valid), .pkt_seq(pkt_seq),
        .pkt_count(pkt_count), .seq_gap(seq_gap)
    );

    itch_parser #(.DATA_W(DATA_W)) u_itch (
        .clk(clk), .rst(rst),
        .s_axis_tdata(m_td), .s_axis_tkeep(m_tk), .s_axis_tvalid(m_tv),
        .s_axis_tready(m_tr), .s_axis_tlast(m_tl),
        .msg_valid(msg_valid), .msg_modeled(msg_modeled), .msg_type(msg_type),
        .msg_timestamp(msg_timestamp), .msg_order_ref(msg_order_ref),
        .msg_new_order_ref(msg_new_order_ref), .msg_side(msg_side),
        .msg_shares(msg_shares), .msg_price(msg_price), .msg_stock(msg_stock)
    );

`ifndef SYNTHESIS
    initial begin
        $dumpfile("feed_top.vcd");
        $dumpvars(0, feed_top);
    end
`endif

endmodule
