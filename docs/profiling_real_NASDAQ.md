# Profiling real NASDAQ data .. (7.7 GB)

## Method
-     ./build/itch_book_replay ~/market-data/12302019.NASDAQ_ITCH50 AAPL


=== ITCH replay: AAPL ===

modeled messages parsed : 263241937

adds=698728

deletes=654441

reduces=62010

replaces=96968

final book state for AAPL:
live orders   : 0

bid levels    : 0

ask levels: 0
  (one side empty — symbol may be wrong, or slice too short to populate both sides)

 Performance counter stats for './build/itch_book_replay /market-data/12302019.NASDAQ_ITCH50 AAPL':

            21,024      context-switches                 #   1414.4 cs/sec  cs_per_second     
                 0      cpu-migrations                   #      0.0 migrations/sec  migrations_per_second
          1,25,671      page-faults                      #   8454.5 faults/sec  page_faults_per_second
         14,864.37 msec task-clock                       #      0.9 CPUs  CPUs_utilized       
      36,80,69,673      cpu_core/L1-dcache-load-misses/  #      nan %  l1d_miss_rate            (80.02%)
      20,73,74,715      cpu_core/LLC-loads/              #     51.1 %  llc_miss_rate            (79.95%)
      29,46,70,027      cpu_core/branch-misses/          #      1.7 %  branch_miss_rate         (69.64%)
      17,24,51,22,468      cpu_core/branches/            #   1160.2 M/sec  branch_frequency     (69.83%)
      61,84,44,67,219      cpu_core/cpu-cycles/          #      4.2 GHz  cycles_frequency       (70.12%)
      87,76,77,02,995      cpu_core/instructions/        #      1.4 instructions  insn_per_cycle  (70.20%)
      <not counted>      cpu_atom/LLC-loads/             #      nan %  llc_miss_rate            (0.00%)
      <not counted>      cpu_atom/branch-misses/         #      nan %  branch_miss_rate         (0.00%)
      <not counted>      cpu_atom/branches/              #      nan M/sec  branch_frequency     (0.00%)
      <not counted>      cpu_atom/cpu-cycles/            #      nan GHz  cycles_frequency       (0.00%)
      <not counted>      cpu_atom/instructions/          #      nan instructions  insn_per_cycle  (0.00%)
               TopdownL1 (cpu_core)                      #      2.5 %  tma_bad_speculation    
                                                         #     13.1 %  tma_frontend_bound       (80.04%)
                                                         #     69.4 %  tma_backend_bound      
                                                         #     14.9 %  tma_retiring             (80.04%)
              TopdownL1 (cpu_atom)                       #      nan %  tma_backend_bound        (0.00%)
                                                         #      nan %  tma_frontend_bound       (0.00%)
                                                         #      nan %  tma_bad_speculation    
                                                         #      nan %  tma_retiring             (0.00%)

      16.462599885 seconds time elapsed

       5.197321000 seconds user
       9.673235000 seconds sys