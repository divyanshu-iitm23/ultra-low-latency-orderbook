// axis_reg.v - AXI-Stream register slice (skid buffer)
//

module axis_reg #(
    parameter DATA_W = 64
) (
    input  wire                clk,
    input  wire                rst,            // synchronous, active-high

    // slave side (upstream feeds us)
    input  wire [DATA_W-1:0]   s_axis_tdata,
    input  wire [DATA_W/8-1:0] s_axis_tkeep,
    input  wire                s_axis_tvalid,
    output wire                s_axis_tready,
    input  wire                s_axis_tlast,

    // master side (we feed downstream)
    output wire [DATA_W-1:0]   m_axis_tdata,
    output wire [DATA_W/8-1:0] m_axis_tkeep,
    output wire                m_axis_tvalid,
    input  wire                m_axis_tready,
    output wire                m_axis_tlast
);

    // output stage
    reg [DATA_W-1:0]   data_q;
    reg [DATA_W/8-1:0] keep_q;
    reg                last_q;
    reg                valid_q;

    // skid stage: holds the beat accepted on the cycle the output stalled
    reg [DATA_W-1:0]   skid_data_q;
    reg [DATA_W/8-1:0] skid_keep_q;
    reg                skid_last_q;
    reg                skid_valid_q;

    assign s_axis_tready = !skid_valid_q;      // can accept unless skid is full

    assign m_axis_tdata  = data_q;
    assign m_axis_tkeep  = keep_q;
    assign m_axis_tlast  = last_q;
    assign m_axis_tvalid = valid_q;

    always @(posedge clk) begin
        if (rst) begin
            valid_q      <= 1'b0;
            skid_valid_q <= 1'b0;
        end else begin
            if (m_axis_tready || !valid_q) begin
                // output stage advances: drain the skid first, else take input
                if (skid_valid_q) begin
                    data_q       <= skid_data_q;
                    keep_q       <= skid_keep_q;
                    last_q       <= skid_last_q;
                    valid_q      <= 1'b1;
                    skid_valid_q <= 1'b0;
                end else begin
                    data_q  <= s_axis_tdata;
                    keep_q  <= s_axis_tkeep;
                    last_q  <= s_axis_tlast;
                    valid_q <= s_axis_tvalid;
                end
            end else if (s_axis_tvalid && s_axis_tready) begin
                // output stalled but a beat was already accepted: park it
                skid_data_q  <= s_axis_tdata;
                skid_keep_q  <= s_axis_tkeep;
                skid_last_q  <= s_axis_tlast;
                skid_valid_q <= 1'b1;
            end
        end
    end

`ifndef SYNTHESIS
    initial begin
        $dumpfile("axis_reg.vcd");
        $dumpvars(0, axis_reg);
    end
`endif

endmodule
