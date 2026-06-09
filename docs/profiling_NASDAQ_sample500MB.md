# Profiling real NASDAQ data (small part of it) .. 500MB

## Method
-     ./build/itch_book_replay ~/market-data/sample_500mb.ITCH50 AAPL 


=== ITCH replay: AAPL ===

modeled messages parsed : 13838945

adds=50364

deletes=26200

reduces=3660

replaces=4948

final book state for AAPL:
live orders   : 21948

bid levels    : 3597   ask levels: 1135

best bid      : $287.53

best ask      : $287.58

spread        : $0.05

crossed?      : no (good)

 Performance counter stats for './build/itch_book_replay /market-data/sample_500mb.ITCH50 AAPL':

                13      context-switches                 #     41.4 cs/sec  cs_per_second     
                 0      cpu-migrations                   #      0.0 migrations/sec  migrations_per_second
            49,997      page-faults                      # 159305.9 faults/sec  page_faults_per_second
            313.84 msec task-clock                       #      1.0 CPUs  CPUs_utilized       
         79,50,244      cpu_core/L1-dcache-load-misses/  #      nan %  l1d_miss_rate            (79.62%)
         35,78,458      cpu_core/LLC-loads/              #     37.2 %  llc_miss_rate            (79.62%)
       1,05,82,331      cpu_core/branch-misses/          #      4.0 %  branch_miss_rate         (69.41%)
      26,42,88,246      cpu_core/branches/               #    842.1 M/sec  branch_frequency     (69.42%)
    1,28,57,18,338      cpu_core/cpu-cycles/             #      4.1 GHz  cycles_frequency       (69.47%)
    1,42,35,61,496      cpu_core/instructions/           #      1.1 instructions  insn_per_cycle  (69.85%)
     <not counted>      cpu_atom/LLC-loads/              #      nan %  llc_miss_rate            (0.00%)
     <not counted>      cpu_atom/branch-misses/          #      nan %  branch_miss_rate         (0.00%)
     <not counted>      cpu_atom/branches/               #      nan M/sec  branch_frequency     (0.00%)
     <not counted>      cpu_atom/cpu-cycles/             #      nan GHz  cycles_frequency       (0.00%)
     <not counted>      cpu_atom/instructions/           #      nan instructions  insn_per_cycle  (0.00%)
             TopdownL1 (cpu_core)                        #     15.5 %  tma_bad_speculation    
                                                         #     13.8 %  tma_frontend_bound       (80.09%)
                                                         #     51.0 %  tma_backend_bound      
                                                         #     19.6 %  tma_retiring             (80.09%)
             TopdownL1 (cpu_atom)                        #      nan %  tma_backend_bound        (0.00%)
                                                         #      nan %  tma_frontend_bound       (0.00%)
                                                         #      nan %  tma_bad_speculation    
                                                         #      nan %  tma_retiring             (0.00%)

       0.318320305 seconds time elapsed

       0.234964000 seconds user
       0.079649000 seconds sys
