// itch_parser.v - streaming NASDAQ ITCH 5.0 message parser (BinaryFILE framing)
//
// Input: continuous byte stream over AXI-Stream (64-bit).
// each message is preceded by a 2-byte big-endian length.
// Output: one decoded-message strobe per message.
// tready is always high, up to 8 bytes consumed per cycle.


module itch_parser #(
    parameter DATA_W  = 64,
    parameter MAX_MSG = 50                     // largest ITCH 5.0 body
) (
    input  wire                clk,
    input  wire                rst,            // synchronous, active-high

    input  wire [DATA_W-1:0]   s_axis_tdata,
    input  wire [DATA_W/8-1:0] s_axis_tkeep,
    input  wire                s_axis_tvalid,
    output wire                s_axis_tready,
    input  wire                s_axis_tlast,   // unused: the stream is continuous

    output reg                 msg_valid,      // 1-cycle strobe per message
    output wire                msg_modeled,    // type in {A,F,E,C,X,D,U} at full length
    output wire [7:0]          msg_type,
    output wire [47:0]         msg_timestamp,  // ns since midnight
    output wire [63:0]         msg_order_ref,
    output wire [63:0]         msg_new_order_ref,  // 'U' only
    output wire [7:0]          msg_side,           // 'A'/'F' only
    output wire [31:0]         msg_shares,
    output wire [31:0]         msg_price,          // 1/10000 dollar
    output wire [63:0]         msg_stock           // raw 8 chars, first char in [63:56]
);

    localparam [1:0] S_LEN0 = 2'd0,            // awaiting length high byte
                     S_LEN1 = 2'd1,            // awaiting length low byte
                     S_BODY = 2'd2;            // accumulating message body

    reg [1:0]           state;
    reg [15:0]          mlen, mcnt;
    reg [8*MAX_MSG-1:0] mbuf;

    // completed message, held stable while msg_valid strobes
    reg [8*MAX_MSG-1:0] dec_buf;
    reg [15:0]          dec_len;

    assign s_axis_tready = 1'b1;

    // walk the beat's byte lanes in order; blocking temps carry the FSM
    // through all 8 bytes within one clock
    integer i;
    reg [1:0]           st;
    reg [15:0]          len_v, cnt_v;
    reg [8*MAX_MSG-1:0] buf_v;
    reg [8*MAX_MSG-1:0] dec_v;
    reg [15:0]          dlen_v;
    reg                 got_v;
    reg [7:0]           b;

    always @(posedge clk) begin
        if (rst) begin
            state     <= S_LEN0;
            mlen      <= 16'd0;
            mcnt      <= 16'd0;
            mbuf      <= {8*MAX_MSG{1'b0}};
            dec_buf   <= {8*MAX_MSG{1'b0}};
            dec_len   <= 16'd0;
            msg_valid <= 1'b0;
        end else begin
            msg_valid <= 1'b0;
            if (s_axis_tvalid) begin
                st     = state;
                len_v  = mlen;
                cnt_v  = mcnt;
                buf_v  = mbuf;
                dec_v  = dec_buf;
                dlen_v = dec_len;
                got_v  = 1'b0;
                for (i = 0; i < DATA_W/8; i = i + 1) begin
                    if (s_axis_tkeep[i]) begin
                        b = s_axis_tdata[8*i +: 8];
                        case (st)
                            S_LEN0: begin
                                len_v[15:8] = b;
                                st = S_LEN1;
                            end
                            S_LEN1: begin
                                len_v[7:0] = b;
                                cnt_v = 16'd0;
                                // len==0 is end-of-file padding: skip it
                                st = (len_v == 16'd0) ? S_LEN0 : S_BODY;
                            end
                            default: begin      // S_BODY
                                if (cnt_v < MAX_MSG)
                                    buf_v[8*cnt_v +: 8] = b;
                                cnt_v = cnt_v + 16'd1;
                                if (cnt_v == len_v) begin
                                    // snapshot HERE: later lanes of this same
                                    // beat may already start the next message
                                    dec_v  = buf_v;
                                    dlen_v = len_v;
                                    st     = S_LEN0;
                                    got_v  = 1'b1;
                                end
                            end
                        endcase
                    end
                end
                state <= st;
                mlen  <= len_v;
                mcnt  <= cnt_v;
                mbuf  <= buf_v;
                if (got_v) begin
                    dec_buf   <= dec_v;
                    dec_len   <= dlen_v;
                    msg_valid <= 1'b1;
                end
            end
        end
    end

    // combinational field extraction from the completed message
    // byte view of dec_buf: db[0] is the message type
    wire [7:0] db [0:MAX_MSG-1];
    genvar g;
    generate
        for (g = 0; g < MAX_MSG; g = g + 1) begin : BYTES
            assign db[g] = dec_buf[8*g +: 8];
        end
    endgenerate

    wire [7:0] t = db[0];

    wire known = (t == "A") || (t == "F") || (t == "E") || (t == "C")
              || (t == "X") || (t == "D") || (t == "U");

    wire [15:0] minlen = (t == "A") ? 16'd36 :
                         (t == "F") ? 16'd40 :
                         (t == "E") ? 16'd31 :
                         (t == "C") ? 16'd36 :
                         (t == "X") ? 16'd23 :
                         (t == "D") ? 16'd19 :
                         (t == "U") ? 16'd35 : 16'hFFFF;

    wire full = known && (dec_len >= minlen);

    wire [47:0] ts_raw  = {db[5], db[6], db[7], db[8], db[9], db[10]};
    wire [63:0] ref_raw = {db[11], db[12], db[13], db[14],
                           db[15], db[16], db[17], db[18]};

    assign msg_modeled = full;
    assign msg_type    = t;

    // golden model: modeled-but-truncated types return before reading the
    // timestamp; unknown types read it whenever len >= 11
    assign msg_timestamp = full                        ? ts_raw :
                           (!known && dec_len >= 16'd11) ? ts_raw : 48'd0;

    assign msg_order_ref = full ? ref_raw : 64'd0;

    assign msg_new_order_ref = (full && t == "U")
        ? {db[19], db[20], db[21], db[22], db[23], db[24], db[25], db[26]}
        : 64'd0;

    assign msg_side = (full && (t == "A" || t == "F")) ? db[19] : 8'd0;

    assign msg_shares = !full ? 32'd0 :
        (t == "A" || t == "F")             ? {db[20], db[21], db[22], db[23]} :
        (t == "E" || t == "C" || t == "X") ? {db[19], db[20], db[21], db[22]} :
        (t == "U")                         ? {db[27], db[28], db[29], db[30]} : 32'd0;

    assign msg_price = !full ? 32'd0 :
        (t == "A" || t == "F" || t == "C") ? {db[32], db[33], db[34], db[35]} :
        (t == "U")                         ? {db[31], db[32], db[33], db[34]} : 32'd0;

    assign msg_stock = (full && (t == "A" || t == "F"))
        ? {db[24], db[25], db[26], db[27], db[28], db[29], db[30], db[31]}
        : 64'd0;

`ifndef SYNTHESIS
    initial begin
        $dumpfile("itch_parser.vcd");
        $dumpvars(0, itch_parser);
    end
`endif

endmodule
