// axis_skip_align.v - the "gearbox": drop N leading bytes of a frame
// then realign the remainder back to lane 0.
// Every protocol layer above this needs the same thing

module axis_skip_align #(
    parameter DATA_W = 64
) (
    input  wire                clk,
    input  wire                rst,             // synchronous, active-high

    input  wire [15:0]         skip,
    input  wire [15:0]         keep_len,
    input  wire                drop,

    input  wire [DATA_W-1:0]   s_axis_tdata,
    input  wire [DATA_W/8-1:0] s_axis_tkeep,
    input  wire                s_axis_tvalid,
    output wire                s_axis_tready,
    input  wire                s_axis_tlast,

    output reg  [DATA_W-1:0]   m_axis_tdata,
    output reg  [DATA_W/8-1:0] m_axis_tkeep,
    output reg                 m_axis_tvalid,
    input  wire                m_axis_tready,
    output reg                 m_axis_tlast
);

    localparam KEEP_W = DATA_W/8;               // 8 bytes per beat
    localparam BUF_B  = 2*KEEP_W;               // 16 bytes of staging

    reg [8*BUF_B-1:0] buf_q;                    // byte i at [8i +: 8]
    reg [4:0]         cnt_q;                    // bytes staged (0..16)

    reg               in_frame;                 // past the first beat of a frame
    reg               drop_q, bounded_q, done_q;
    reg [15:0]        skip_q, left_q;

    wire first_beat = s_axis_tvalid && !in_frame;

    // per-frame control: from the ports on beat 0, from registers thereafter
    wire [15:0] skip_c    = first_beat ? skip            : skip_q;
    wire [15:0] left_c    = first_beat ? keep_len        : left_q;
    wire        bounded_c = first_beat ? (keep_len != 0) : bounded_q;
    wire        drop_c    = first_beat ? drop            : drop_q;
    wire        done_c    = first_beat ? 1'b0            : done_q;

    // a finished payload still sitting in the buffer must go out before the
    // next frame is allowed in (it would overwrite the per-frame control regs)
    wire residue = done_q && (cnt_q != 0);
    wire out_free = !m_axis_tvalid || m_axis_tready;

    assign s_axis_tready = !residue && (cnt_q <= BUF_B - KEEP_W) && out_free;

    wire beat = s_axis_tvalid && s_axis_tready;

    // gather: append this beat's payload bytes
    integer j;
    reg [8*BUF_B-1:0] nbuf;
    reg [4:0]         ncnt;
    reg [15:0]        nskip, nleft;
    reg               ndone;
    reg [7:0]         byte_j;

    always @(*) begin
        nbuf  = buf_q;
        ncnt  = cnt_q;
        nskip = skip_c;
        nleft = left_c;
        ndone = done_c;

        if (beat) begin
            for (j = 0; j < KEEP_W; j = j + 1) begin
                if (s_axis_tkeep[j]) begin
                    byte_j = s_axis_tdata[8*j +: 8];
                    if (nskip != 0) begin
                        nskip = nskip - 16'd1;             // still in the header
                    end else if (!drop_c && (!bounded_c || nleft != 0)) begin
                        nbuf[8*ncnt +: 8] = byte_j;        // payload -> buffer
                        ncnt  = ncnt + 5'd1;
                        if (bounded_c) nleft = nleft - 16'd1;
                    end
                    // else: dropped frame, or past keep_len -- discard
                end
            end
            // no further payload can arrive once the budget is spent or the
            // frame ends -- this is what lets tlast fire early on truncation
            if ((bounded_c && nleft == 0) || s_axis_tlast)
                ndone = 1'b1;
        end
    end

    // emit
    integer m;
    reg [8*BUF_B-1:0] ebuf;
    reg [4:0]         ecnt;

    always @(posedge clk) begin
        if (rst) begin
            buf_q         <= {8*BUF_B{1'b0}};
            cnt_q         <= 5'd0;
            in_frame      <= 1'b0;
            drop_q        <= 1'b0;
            bounded_q     <= 1'b0;
            done_q        <= 1'b0;
            skip_q        <= 16'd0;
            left_q        <= 16'd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            m_axis_tdata  <= {DATA_W{1'b0}};
            m_axis_tkeep  <= {KEEP_W{1'b0}};
        end else begin
            if (m_axis_tvalid && m_axis_tready)
                m_axis_tvalid <= 1'b0;                     // beat consumed

            ebuf = nbuf;
            ecnt = ncnt;

            if (out_free && !drop_c) begin
                if (ecnt >= KEEP_W) begin                  // a full aligned beat
                    for (m = 0; m < KEEP_W; m = m + 1) begin
                        m_axis_tdata[8*m +: 8] <= ebuf[8*m +: 8];
                        m_axis_tkeep[m]        <= 1'b1;
                    end
                    ebuf = ebuf >> (8*KEEP_W);
                    ecnt = ecnt - KEEP_W;
                    m_axis_tvalid <= 1'b1;
                    m_axis_tlast  <= ndone && (ecnt == 0); // this beat empties it
                end else if (ndone && ecnt != 0) begin     // short tail beat
                    for (m = 0; m < KEEP_W; m = m + 1) begin
                        m_axis_tdata[8*m +: 8] <= ebuf[8*m +: 8];
                        m_axis_tkeep[m]        <= (m < ecnt);
                    end
                    ecnt = 5'd0;
                    m_axis_tvalid <= 1'b1;
                    m_axis_tlast  <= 1'b1;
                end
            end

            buf_q <= ebuf;
            cnt_q <= ecnt;

            if (beat) begin
                skip_q    <= nskip;
                left_q    <= nleft;
                bounded_q <= bounded_c;
                drop_q    <= drop_c;
                done_q    <= ndone;
                // the input frame's end is what starts the next frame, even if
                // this one's payload finished earlier (keep_len truncation)
                in_frame  <= !s_axis_tlast;
                if (s_axis_tlast) begin
                    skip_q <= 16'd0;
                    left_q <= 16'd0;
                end
            end
        end
    end

`ifndef SYNTHESIS
    initial begin
        $dumpfile("axis_skip_align.vcd");
        $dumpvars(0, axis_skip_align);
    end
`endif

endmodule
