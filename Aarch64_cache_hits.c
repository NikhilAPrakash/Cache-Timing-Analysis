#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/mman.h>
#include <sys/stat.h>

#define L1_CACHE_SIZE_IN_BYTES 32768
#define L2_CACHE_SIZE_IN_BYTES 2097152
#define NUM_OF_TRACES 1000000

// DO NOT MOVE THIS FROM HERE, EITHER UP or DOWN.
// IT IS EXACTLY WHERE IT IS SUPPOSED TO BE
// IT HAS NO PURPOSE IN THE CODE, YET IT SERVES PURPOSE FOR THE INTENDED RESULT
int MAGIC_BUFFER[4037] = {4};

volatile int L1_buffer[L1_CACHE_SIZE_IN_BYTES] = {0};
volatile int L2_buffer[L2_CACHE_SIZE_IN_BYTES] = {0};

int L1_eviction_buffer[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

FILE *L1_cache_hit_file, *L2_cache_hit_file, *SDRAM_hit_file;
uint64_t *L1_hit_times, *L2_hit_times, *SDRAM_hit_times, *L1_data_at_times, *L2_data_at_times, *SDRAM_data_at_times;

uint64_t *timing;

void init();
static inline void enable_cycle_counter(void);
void doSDRAMTrace();
void doL1Trace();
void doL2Trace();
void write_times_to_file();

int main()
{
    printf("begin\n");
    init();

    enable_cycle_counter();

    printf("Collecting data\n");

    doSDRAMTrace();
    doL2Trace();
    doL1Trace();

    write_times_to_file();

    free(L1_hit_times);
    free(L2_hit_times);
    free(SDRAM_hit_times);

    free(L1_data_at_times);
    free(L2_data_at_times);
    free(SDRAM_data_at_times);

    fclose(L1_cache_hit_file);
    fclose(L2_cache_hit_file);
    fclose(SDRAM_hit_file);

    printf("Done\n");
}

// using PMU Timer
// Source: https://zhiyisun.github.io/2016/03/02/How-to-Use-Performance-Monitor-Unit-(PMU)-of-64-bit-ARMv8-A-in-Linux.html
static inline void enable_cycle_counter(void)
{
    unsigned int value;
    // Enable user access to PMU
    asm volatile("mrs %0, pmcr_el0\n\t"
                 "orr %0, %0, #1\n\t" // Enable Performance Monitor Counter
                 "msr pmcr_el0, %0\n\t"
                 : "=r"(value));
    asm volatile("msr pmcntenset_el0, %0" ::"r"(1 << 31)); // Enable Cycle Counter
}

void doSDRAMTrace()
{
    unsigned cycles_start, cycles_stop;
    uint64_t time_for_misses = 0;
    volatile void *target_SDRAM = &L1_buffer[32767];
    volatile int data_at_mem = -1;

    // SD-RAM  hits
    for (int i = 0; i < NUM_OF_TRACES; ++i)
    {

        // The sole purpose of this serializing instruction is to DRAIN the STORE BUFFER
        // this will make there ISN'T any kind of store forwarding when we perform a load operation later on
        asm volatile("isb"
                     :
                     :
                     : "memory");

        // write the data to memory if the dirty bit was set AND
        // Invalidate the cache line at every cache level, indexed by target1
        // The memory clobber stops the compiler from optimizing/re-ordering the instruction, this is a compiler level memory barrier
        asm volatile(
            "DC CIVAC, %0"
            :
            : "r"(target_SDRAM)
            : "memory");

        // START THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0"
            : "=r"(cycles_start)
            :
            :);

        // bring the data at memory target1 back into the CPU.
        // Since the cacheline was invalidated earlier, hence,
        // the read/load will pull data in from SD-RAM instead of cache
        asm volatile(
            "LDR %0, [%1]\n\t"
            : "=&r"(data_at_mem)
            : "r"(target_SDRAM)
            :); // we access the target AGAIN, but this time the data comes from L1 cache

        // STOP THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0\n\t"
            "isb"
            : "=r"(cycles_stop)
            :
            : "memory" // No clobbered registers
        );

        *timing = cycles_stop - cycles_start;

        time_for_misses += *timing;
        SDRAM_hit_times[i] = *timing;
        SDRAM_data_at_times[i] = data_at_mem;
    }

    printf("Average time for SD-RAM hits: %lu cycles\n", time_for_misses / NUM_OF_TRACES);
}

void doL1Trace()
{
    unsigned cycles_start, cycles_stop;
    uint64_t time_for_hits = 0;
    volatile void *mem_addr = NULL;
    // volatile void* target_L1 = &L1_buffer[7168];
    //0000 - 4095
    //4096 - 8191
    //8192 - 12287
    //12288 - 16383
    //16384 - 20479
    //20480 - 24575
    //24576 - 28671
    //28672 - 32767 ->>>>>> INTERESTED
    volatile void *target_L1 = &L1_buffer[28672];
    volatile int data_at_mem = -1;

    // the sole purpose of this loop is to avoid descrepancies in the initial readings
    // its an exact replica of the below loop, for comments go to the below loop
    for (int i = 0; i < NUM_OF_TRACES / 10; ++i)
    {
        for (int num_of_iter = 0; num_of_iter < 2; ++num_of_iter)
        {
            for (int stride = 0; stride < 8192; stride = stride + 4096)
            {
                mem_addr = &L1_buffer[stride];
                asm volatile(
                    "LDR %0, [%1]\n\t"
                    "DSB LD\n\t"
                    "ISB\n\t"
                    : "=&r"(data_at_mem)
                    : "r"(mem_addr)
                    : "memory");
            }

            asm volatile("isb\n\t" :);
        }

        // START THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0"
            : "=r"(cycles_start)
            :
            :);

        asm volatile(
            "LDR %0, [%1]\n\t"
            : "=&r"(data_at_mem)
            : "r"(target_L1)
            :); // we access the target AGAIN, but this time the data comes from L1 cache

        // STOP THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0\n\t"
            "isb"
            : "=r"(cycles_stop)
            :
            : "memory");

        *timing = cycles_stop - cycles_start;

        L1_hit_times[i] = *timing;
        L1_data_at_times[i] = data_at_mem;
    }

    // L1 cache hits
    for (int i = 0; i < NUM_OF_TRACES; ++i)
    {

        // if I pull in integer at index 0
        // then cpu will also pull in integers at indexes 0,1,2,.....,14,15
        // all 16 integers will be a part of A single cache line

        // The below for loop is basically filling up the FIRST SET ONLY
        // I am repeatedly pulling in data from addresses that map to the SAME SET, hence stride of 1024 integers (4096 bytes)
        // The L1 cache size on my machine is 32768 bytes, has 8-way associativity, has 64bytes cacheline, giving me 64 sets
        // need first 6 bits in the address to uniquely identify a byte in a cacheline
        // the next 6 bits in the address are used to uniquely identify a set within L1 data cache
        // as you can see in the addresses below, the set is 0

        //16 integers * 256 sets * 2 associativity -> 8192 integers required to COMPLETELY fill the L1 cache one time
        //16 integers * 256 sets -> (0000 - 4095) 4096 integers required to fill first cache line of ALL sets in L1 cache
        //                       -> (4096 - 8191) 4096 integers required to fill second cache line of ALL sets in L1 cache
        for (int num_of_iter = 0; num_of_iter < 4; ++num_of_iter) {

            int stride_start = 8192 * num_of_iter;
            for (int stride = stride_start; stride < (stride_start + 8192); stride = stride + 4096) {
                mem_addr = &L1_buffer[stride];

                // an instruction that loads from memory AND that precedes an LFENCE, RECEIVES data from memory PRIOR to completion of the LFENCE
                // With the help of LFENCE, we are making sure that the data indexed by target2 is indeed present within the L1 cache
                // The memory clobber stops the compiler from optimizing/re-ordering the instruction, this is a compiler level memory barrier
                asm volatile(
                    "LDR %0, [%1]\n\t"
                    "DSB SY\n\t"
                    "ISB\n\t"
                    : "=&r"(data_at_mem)
                    : "r"(mem_addr)
                    : "memory");
            }

            asm volatile("isb\n\t"
                        :
                        :
                        : "memory" );
        }

        // START THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0"
            : "=r"(cycles_start)
            :
            :);

        asm volatile(
            "LDR %0, [%1]\n\t"
            : "=&r"(data_at_mem)
            : "r"(target_L1)
            : "memory"); // we access the target AGAIN, but this time the data comes from L1 cache

        // STOP THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0\n\t"
            "isb"
            : "=r"(cycles_stop)
            :
            : "memory");

        *timing = cycles_stop - cycles_start;

        time_for_hits += *timing;
        L1_hit_times[i] = *timing;
        L1_data_at_times[i] = data_at_mem;
    }

    printf("Average time for L1 cache hit: %lu cycles\n", time_for_hits / NUM_OF_TRACES);
}

void doL2Trace()
{

    unsigned cycles_start, cycles_stop;
    uint64_t time_for_hits = 0;
    volatile void *mem_addr = NULL;
    //000000 - 032767
    //032768 - 065535
    //065536 - 098303
    //098304 - 131071
    //131072 - 163839
    //163840 - 196607
    //196608 - 229375
    //229376 - 262143
    //262144 - 294911
    //294912 - 327679
    //327680 - 360447
    //360448 - 393215
    //393216 - 425983
    //425984 - 458751
    //458752 - 491519
    //491520 - 524287
    volatile void *target_L2 = &L1_buffer[7168];
    volatile int data_at_mem = -1;

    // L2 cache hits
    for (int i = 0; i < NUM_OF_TRACES; ++i)
    {

        // make sure that the entire data is indeed in L2 cache AND
        // the L1 cache as well is completely filled with our data
        // using the stride of 16 since an integer is 4 bytes and the cacheline is 64 bytes
        // even tho we call only integer at pos x, the CPU fills the cache line with x, x+1, x+2 .... x+14, x+15
        // The function of this loop is to fill the cache "SET by SET". Meaning,
        // in the first iteration, the first cache line of FIRST set will be filled, then
        // in the second iteration, the first cache line of SECOND set will be filled, then
        // in the third iteration, the first cache line of THIRD set will be filled, and so on

        // first 1024 integers will fill up the first cache line of each set on my L1 data cache
        // I need 8192 integers to fill up the every cache line within every set on my L1 data cache
        // 0th - 15th integer will fill the first cache line of first set
        // 16th - 31st integer will fill the first cache line of second set
        // 32nd - 47th integer will fill the first cache line of third set
        //......
        //.....
        // 1008th - 1023th integer will fill the first cache line of 64th set

        // fill in the L1 cache
        // the looop goes from 0 to 32768
        // i have 32768 integers
        // my first 8192 + 8192 + 8192 + 8192 = 32768 integers
        // 8192 integers = 32768 bytes of data -> size of L1 cache

        // 16 ints x 1024 sets x 16 assoc -> 262144 ints needed to fill the entire L2 cache
        // Another way to look at 262144 is as a multiple
        // Multiple??? What do I mean??
        // 262144 * 0 = 0 , 262144 * 1 = 262144 , 262144 * 2 = 524288 , 262144 * 3 = 786432 , 262144 * 4 = 1048576
        // Through the above information we discover that:
        // 1. 000000 - 262143 integers are used to completely fill the L2 cache first time
        // 2. 262144 - 524287 integers are used to completely fill the L2 cache second time
        // 3. 524288 - 786431 integers are used to completely fill the L2 cache third time
        // 4. 786432 - 1048575 integers are used to completely fill the L2 cache fourth time
        // This same logic could be applied to "A" set of the L2 cache as well

        // Through the below loop, we are targetting only a specific set, in the L2 AND L1 cache
        // such that the cache line in L1 gets overwritten with new integers, however, still being present in L2 cache

        //16 integers * 2048 sets * 16 associativity -> 524288 integers required to COMPLETELY fill the L2 cache one time
        //16 integers * 2048 sets -> (0 - 32767) 32768 integers required to fill first cache line of ALL sets in L2 cache
        //                        -> (32768 - 65535) 32768 integers required to fill second cache line of ALL sets in L2 cache
        //                        ......
        //                        .....
        //                        ...
        //                        -> (491520 - 524287) 32768 integers required to fill 16th cache line of ALL sets in L2 cache
        for (int num_of_iter = 0; num_of_iter < 4; ++num_of_iter) {

            int stride_start = 524288 * num_of_iter;
            for (int stride = stride_start; stride < (stride_start + 524288); stride = stride + 32768)
            {
                mem_addr = &L2_buffer[stride];

                // an instruction that loads from memory AND that precedes an LFENCE, RECEIVES data from memory PRIOR to completion of the LFENCE
                // With the help of LFENCE, we are making sure that the data indexed by target2 is indeed present within the L1 cache
                // The memory clobber stops the compiler from optimizing/re-ordering the instruction, this is a compiler level memory barrier
                asm volatile(
                    "LDR %0, [%1]\n\t"
                    "DSB SY\n\t"         // Data Synchronization Barrier to ensure visibility of memory operations
                    "ISB\n\t"            // Instruction Synchronization Barrier to synchronize the instruction stream
                    : "=&r"(data_at_mem) // Output: store the loaded data
                    : "r"(mem_addr)      // Input: memory address to load from
                    : "memory"           // Clobber: prevents reordering of memory operations
                );
            }

            asm volatile("isb\n\t" :);
        }

        // START THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0"
            : "=r"(cycles_start)
            :
            :);

        asm volatile(
            "LDR %0, [%1]\n\t"
            : "=&r"(data_at_mem)
            : "r"(&L2_buffer[32768 * (data_at_mem % 16)])
            : "memory");

        // STOP THE TIMER
        asm volatile(
            "isb\n\t"
            "mrs %0, pmccntr_el0\n\t"
            "isb"
            : "=r"(cycles_stop)
            :
            : "memory");

        *timing = cycles_stop - cycles_start;

        time_for_hits += *timing;
        L2_hit_times[i] = *timing;
        L2_data_at_times[i] = data_at_mem;
    }

    printf("Average time for L2 cache hit: %lu cycles\n", time_for_hits / NUM_OF_TRACES);
}

void write_times_to_file()
{
    for (int i = 0; i < NUM_OF_TRACES; i++)
    {
        // fprintf(cache_hit_file, "%llu\n", hit_times[i]);
        fprintf(L1_cache_hit_file, "%lu \t\t\t %ld\n", L1_hit_times[i], L1_data_at_times[i]);
        fprintf(L2_cache_hit_file, "%lu \t\t\t %ld\n", L2_hit_times[i], L2_data_at_times[i]);
        fprintf(SDRAM_hit_file, "%lu \t\t\t %ld\n", SDRAM_hit_times[i], SDRAM_data_at_times[i]);
    }
}

void init()
{

    srand(time(NULL));
    for (int dataPtr = 0; dataPtr < L1_CACHE_SIZE_IN_BYTES; ++dataPtr)
    {
        L1_buffer[dataPtr] = rand() % 32767;
    }
    for (int dataPtr = 0; dataPtr < L2_CACHE_SIZE_IN_BYTES; ++dataPtr)
    {
        L2_buffer[dataPtr] = rand() % 2097151;
    }

    printf("Start+16 of L2 eviction buffer | 1st CL of SET 2 : %p\n", &L1_buffer[16]);
    printf("\n\n\nStart of L2 eviction buffer | 1st CL of SET 1 : %p\n", &L1_buffer[0]);

    printf("Start+1024 of L2 eviction buffer | 2nd CL of SET 1 : %p\n", &L1_buffer[1024]);
    printf("Start+2048 of L2 eviction buffer | 3rd CL of SET 1 : %p\n", &L1_buffer[2048]);
    printf("Start+3072 of L2 eviction buffer | 4th CL of SET 1 : %p\n", &L1_buffer[3072]);
    printf("Start+4096 of L2 eviction buffer | 4th CL of SET 1 : %p\n", &L1_buffer[4096]);
    printf("Start+5120 of L2 eviction buffer | 4th CL of SET 1 : %p\n", &L1_buffer[5120]);
    printf("Start+6144 of L2 eviction buffer | 4th CL of SET 1 : %p\n", &L1_buffer[6144]);
    printf("Start+7168 of L2 eviction buffer | 4th CL of SET 1 : %p\n", &L1_buffer[7168]);
    printf("Start+8192 of L2 eviction buffer | 4th CL of SET 1 : %p\n", &L1_buffer[8192]);

    // setup files pointer for writing
    printf("preparing data collection\n");
    timing = (uint64_t *)malloc(sizeof(uint64_t));
    L1_hit_times = (uint64_t *)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L2_hit_times = (uint64_t *)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    SDRAM_hit_times = (uint64_t *)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L1_data_at_times = (uint64_t *)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L2_data_at_times = (uint64_t *)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    SDRAM_data_at_times = (uint64_t *)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L1_cache_hit_file = fopen("L1_cache_hits.txt", "w");
    L2_cache_hit_file = fopen("L2_cache_hits.txt", "w");
    SDRAM_hit_file = fopen("SDRAM_cache_hits.txt", "w");
}
