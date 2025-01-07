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
#define L2_CACHE_SIZE_IN_BYTES 262144
#define L2_BUFFER_DESIRED_ADDRESS 0x600000
#define L3_CACHE_SIZE_IN_BYTES 12582912
#define NUM_OF_TRACES 1000000

//DO NOT MOVE THIS FROM HERE, EITHER UP or DOWN.
//IT IS EXACTLY WHERE IT IS SUPPOSED TO BE
//IT HAS NO PURPOSE IN THE CODE, YET IT SERVES PURPOSE FOR THE INTENDED RESULT
int MAGIC_BUFFER[1980] = {4};

volatile int L1_buffer[L1_CACHE_SIZE_IN_BYTES] = {0};
volatile int L2_buffer[L2_CACHE_SIZE_IN_BYTES] = {0};
volatile int L3_buffer[L3_CACHE_SIZE_IN_BYTES] = {0};

size_t l2_buffer_size = L2_CACHE_SIZE_IN_BYTES;

FILE *L1_cache_hit_file, *L2_cache_hit_file, *L3_cache_hit_file, *SDRAM_hit_file;
uint64_t *L1_hit_times, *L2_hit_times, *L3_hit_times, *SDRAM_hit_times, *L1_data_at_times, *L2_data_at_times, *L3_data_at_times, *SDRAM_data_at_times;

uint64_t *timing;

void init();
void doSDRAMTrace();
void doL1Trace();
void doL2Trace();
void doL3Trace();
void write_times_to_file();

int main(){
    printf("begin\n");
    init();

    printf("Collecting data\n");

    doSDRAMTrace();
    doL3Trace();
    doL2Trace();
    doL1Trace();

    write_times_to_file();

    free(L1_hit_times);
    free(L2_hit_times);
    free(L3_hit_times);
    free(SDRAM_hit_times);

    free(L1_data_at_times);
    free(L2_data_at_times);
    free(L3_data_at_times);
    free(SDRAM_data_at_times);

    fclose(L1_cache_hit_file);
    fclose(L2_cache_hit_file);
    fclose(L3_cache_hit_file);
    fclose(SDRAM_hit_file);

    printf("Done\n");
}


/*
The timing ASM functions are taken from the White Paper "How to Benchmark Code Execution Times on Intel® IA-32 and IA-64 Instruction Set Architectures" by "Gabriele Paoloni"
*/

/*
Explaination of used terminologies within the timing ASM instructions:

There is a timestamp counter(register) in Intel that keeps track of the cycles.
This counter can be accessed using "RDTSC" & "RDTSCP" instructions.
On a 64-bit machine, the RDTSC instruction loads the high-order 32-bits of the timestamp register into RDX and low-order 32-bits into RAX.

Furthermore, since almost all modern processors use "Out-of-order execution". Hence,
additional effort must be put from our end in ensuring that TIMING instructions are executed IN-ORDER.
The solution is to call a serializing instruction before calling the RDTSC instruction.
A serializing instruction makes sure that every instruction before it "in program order" is completed(not sure whether executed/committed) AND none after it "in program order" are executed.
CPUID is that serializing instruction. There are other non-privileged serializing instructions as well: IRET, RSM, and SERIALIZE
On a 64-bit machine, the CPUID instruction overwrites the "RAX" , "RBX" , "RCX" , "RDX" instructions. Additionally,
CPUID clears the the high 32-bits of the RAX/RBX/RCX/RDX registers

As the "RAX & RDX" registers are overwritten by RDTSC, AND the registers "RAX" , "RBX" , "RCX" , "RDX" by CPUID instruction.
The compiler has no visibility of these registers being modified, we must explicitly inform the compliler about their modification AND
to also stop the compiler from using these registers for input/output operands, we must specify them as "Clobbered Registers" within our ASM instruction.
Not specifying these registers as clobber registers will give us SEGMENTATION FAULT error

NOTE: A serializing instruction is not same as a memory-ordering instruction (LFENCE, SFENCE & MFENCE). LFENCE, indeed MAKES SURE that
it does not execute until all prior instructions have completed(executed or committed????) locally, and no later instruction begins execution until LFENCE completes.
However, this is not a serialization guarantee
*/

/*
Timing ASM Code explanation:
1. with the help of CPUID, serialize every instruction before the timer start function execution
2. Capture the value of the counter/TSC register
3. store the captured value into RAX & RDX registers
4. access the data (intended cache line)
5. with the help of RDTSCP, make sure that the below two operations take place (POSSIBLY out-of-order):
   - the timer value is stored AND
   - intended cache line is accessed
   After the above two operations complete, capture the value of TSC again. This is a hint that RDTSCP is a compound instruction.
6. store the value captured from the counter/TSC register into RAX & RDX registers
7. call CPUID again, this is done to MAKE SURE that operation in step 6 indeed completes, before we move further
*/

void doSDRAMTrace(){

    unsigned cycles_high_temp, cycles_low_temp, cycles_high_start, cycles_low_start, cycles_high_stop, cycles_low_stop;
    uint64_t time_for_misses = 0;
    volatile void* target_SDRAM = &L1_buffer[32767];
    volatile int data_at_mem = -1;

    //SD-RAM  hits
    for(int i=0; i < NUM_OF_TRACES; ++i){

      //The sole purpose of this serializing instruction is to DRAIN the STORE BUFFER
      //this will make there ISN'T any kind of store forwarding when we perform a load operation later on
      asm volatile ("CPUID\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t"
                  : "=r" (cycles_high_temp), "=r" (cycles_low_temp)
                  :
                  : "%rax", "%rbx", "%rcx", "%rdx", "memory");

      //write the data to memory if the dirty bit was set AND
      //Invalidate the cache line at every cache level, indexed by target1
      //The memory clobber stops the compiler from optimizing/re-ordering the instruction, this is a compiler level memory barrier
      asm volatile("clflush (%0)"
                  :
                  : "r" (target_SDRAM)
                  : "memory");

      //START THE TIMER
      asm volatile ("CPUID\n\t"
                  "RDTSC\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t"
                  : "=r" (cycles_high_start), "=r" (cycles_low_start)
                  :
                  : "%rax", "%rbx", "%rcx", "%rdx");


      //bring the data at memory target1 back into the CPU.
      //Since the cacheline was invalidated earlier, hence,
      //the read/load will pull data in from SD-RAM instead of cache
      asm volatile("mov (%1), %0"
                  : "=&r" (data_at_mem)
                  : "r" (target_SDRAM)
                  : "memory");              //we access the target AGAIN, but this time the data comes from SD-RAM

      //STOP THE TIMER
      //the RDTSCP instruction in the below ASM function will make sure that the LOAD performed in above instruction is globally visible OR
      //the data indexed by target1 is indeed in L1 & L2
      //only then it'll read the TSC register
      asm volatile("RDTSCP\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t"
                  "CPUID\n\t"
                  : "=r" (cycles_high_stop), "=r" (cycles_low_stop)
                  :
                  : "%rax", "%rbx", "%rcx", "%rdx");

      *timing = ((((uint64_t)cycles_high_stop << 32) | cycles_low_stop) - (((uint64_t)cycles_high_start << 32) | cycles_low_start));

      time_for_misses += *timing;
      SDRAM_hit_times[i] = *timing;
      SDRAM_data_at_times[i] = data_at_mem;
    }

    printf("Average time for SD-RAM hits: %llu cycles\n", time_for_misses / NUM_OF_TRACES);
}

void doL1Trace(){

  unsigned cycles_high_start, cycles_low_start, cycles_high_stop, cycles_low_stop;
	uint64_t time_for_hits = 0;
  volatile void* mem_addr = NULL;
  //volatile void* target_L1 = &L1_buffer[7168];
  volatile void* target_L1 = &L1_buffer[31744];
  volatile int data_at_mem = -1;

  //the sole purpose of this loop is to avoid descrepancies in the initial readings
  //its an exact replica of the below loop, for comments go to the below loop
  for(int i=0; i < NUM_OF_TRACES/10; ++i){
      for(int num_of_iter = 0 ; num_of_iter < 2; ++num_of_iter){
          for(int stride = 0; stride < 8192; stride = stride + 1024){
              mem_addr = &L1_buffer[stride];
              asm volatile("mov (%1), %0\n\t"
                          "lfence"
                          : "=&r" (data_at_mem)
                          : "r" (mem_addr)
                          : "memory");
          }

          asm volatile("RDTSCP\n\t"
                      "lfence"
                      :
                      :
                      : "%rax", "%rcx", "%rdx", "memory");
      }

      //START THE TIMER
      asm volatile ("CPUID\n\t"
                  "RDTSC\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t"
                  : "=r" (cycles_high_start), "=r" (cycles_low_start)
                  :
                  : "%rax", "%rbx", "%rcx", "%rdx");

      asm volatile("mov (%1), %0"
                  : "=&r" (data_at_mem)
                  : "r" (target_L1)
                  : );              //we access the target AGAIN, but this time the data comes from L1 cache

      //STOP THE TIMER
      asm volatile("RDTSCP\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t"
                  "CPUID\n\t"
                  : "=r" (cycles_high_stop), "=r" (cycles_low_stop)
                  :
                  : "%rax", "%rbx", "%rcx", "%rdx");

      *timing = ((((uint64_t)cycles_high_stop << 32) | cycles_low_stop) - (((uint64_t)cycles_high_start << 32) | cycles_low_start));

      L1_hit_times[i] = *timing;
      L1_data_at_times[i] = data_at_mem;
  }

	//L1 cache hits
	for(int i=0; i < NUM_OF_TRACES; ++i){

      //if I pull in integer at index 0
      //then cpu will also pull in integers at indexes 0,1,2,.....,14,15
      //all 16 integers will be a part of A single cache line

      //The below for loop is basically filling up the FIRST SET ONLY
      //I am repeatedly pulling in data from addresses that map to the SAME SET, hence stride of 1024 integers (4096 bytes)
      //The L1 cache size on my machine is 32768 bytes, has 8-way associativity, has 64bytes cacheline, giving me 64 sets
      //need first 6 bits in the address to uniquely identify a byte in a cacheline
      //the next 6 bits in the address are used to uniquely identify a set within L1 data cache
      //as you can see in the addresses below, the set is 0

      //Here Start represents the start of the L1_buffer array, i.e. &L1_buffer[0]
      //0x405000 - 0100 0000 0101 0000 0000 0000 - Start+0     - CL 1
      //0x406000 - 0100 0000 0110 0000 0000 0000 - Start+1024  - CL 2
      //0x407000 - 0100 0000 0111 0000 0000 0000 - Start+2048  - CL 3
      //0x408000 - 0100 0000 1000 0000 0000 0000 - Start+3072  - CL 4
      //0x409000 - 0100 0000 1001 0000 0000 0000 - Start+4096  - CL 5
      //0x40a000 - 0100 0000 1010 0000 0000 0000 - Start+5120  - CL 6
      //0x40b000 - 0100 0000 1011 0000 0000 0000 - Start+6144  - CL 7
      //0x40c000 - 0100 0000 1100 0000 0000 0000 - Start+7168  - CL 8
      //0x40d000 - 0100 0000 1101 0000 0000 0000 - Start+8192 -> IGNORE
      for(int num_of_iter = 0 ; num_of_iter < (L1_CACHE_SIZE_IN_BYTES/8192); ++num_of_iter){

          int stride_start = 8192 * num_of_iter;
          for(int stride = stride_start; stride < (stride_start + 8192); stride = stride + 1024){
              mem_addr = &L1_buffer[stride];

              //an instruction that loads from memory AND that precedes an LFENCE, RECEIVES data from memory PRIOR to completion of the LFENCE
              //With the help of LFENCE, we are making sure that the data indexed by target2 is indeed present within the L1 cache
              //The memory clobber stops the compiler from optimizing/re-ordering the instruction, this is a compiler level memory barrier
              asm volatile("mov (%1), %0\n\t"
                          "lfence"
                          : "=&r" (data_at_mem)
                          : "r" (mem_addr)
                          : "memory");
          }

          //From Intel SDM Volume 2B:
          //The RDTSCP instruction is not a serializing instruction, but it does wait until all previous instructions have executed and all previous loads are GLOBALLY VISIBLE.
          //If software requires RDTSCP to be executed prior to execution of any subsequent instruction (including any memory accesses), it can execute LFENCE immediately after RDTSCP
          //Having the guarantee from RDTSCP that the data will be globally visible confirms that data indexed by target2 will indeed be present within the cache
          asm volatile("RDTSCP\n\t"
                      "lfence"
                      :
                      :
                      : "%rax", "%rcx", "%rdx", "memory");
      }

      //START THE TIMER
      asm volatile ("CPUID\n\t"
                  "RDTSC\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t"
                  : "=r" (cycles_high_start), "=r" (cycles_low_start)
                  :
                  : "%rax", "%rbx", "%rcx", "%rdx");

      asm volatile("mov (%1), %0"
                  : "=&r" (data_at_mem)
                  : "r" (target_L1)
                  : "memory" );              //we access the target AGAIN, but this time the data comes from L1 cache

      //STOP THE TIMER
      asm volatile("RDTSCP\n\t"
                  "mov %%edx, %0\n\t"
                  "mov %%eax, %1\n\t"
                  "CPUID\n\t"
                  : "=r" (cycles_high_stop), "=r" (cycles_low_stop)
                  :
                  : "%rax", "%rbx", "%rcx", "%rdx");

      *timing = ((((uint64_t)cycles_high_stop << 32) | cycles_low_stop) - (((uint64_t)cycles_high_start << 32) | cycles_low_start));

  		time_for_hits += *timing;
      L1_hit_times[i] = *timing;
      L1_data_at_times[i] = data_at_mem;
	}

	printf("Average time for L1 cache hit: %llu cycles\n", time_for_hits / NUM_OF_TRACES);
}

void doL2Trace(){

        unsigned cycles_high_start, cycles_low_start, cycles_high_stop, cycles_low_stop;
        uint64_t time_for_hits = 0;
        volatile void* mem_addr = NULL;
        volatile void* target_L2 = &L1_buffer[7168];
        volatile int data_at_mem = -1;

	      //L2 cache hits
        for(int i=0; i < NUM_OF_TRACES; ++i){

            //make sure that the entire data is indeed in L2 cache AND
            //the L1 cache as well is completely filled with our data
            //using the stride of 16 since an integer is 4 bytes and the cacheline is 64 bytes
            //even tho we call only integer at pos x, the CPU fills the cache line with x, x+1, x+2 .... x+14, x+15
            //The function of this loop is to fill the cache "SET by SET". Meaning,
            //in the first iteration, the first cache line of FIRST set will be filled, then
            //in the second iteration, the first cache line of SECOND set will be filled, then
            //in the third iteration, the first cache line of THIRD set will be filled, and so on

            //first 1024 integers will fill up the first cache line of each set on my L1 data cache
            //I need 8192 integers to fill up the every cache line within every set on my L1 data cache
            //0th - 15th integer will fill the first cache line of first set
            //16th - 31st integer will fill the first cache line of second set
            //32nd - 47th integer will fill the first cache line of third set
            //......
            //.....
            //1008th - 1023th integer will fill the first cache line of 64th set

            //fill in the L1 cache
            //the looop goes from 0 to 32768
            //i have 32768 integers
            //my first 8192 + 8192 + 8192 + 8192 = 32768 integers
            //8192 integers = 32768 bytes of data -> size of L1 cache

            //16 ints x 1024 sets x 16 assoc -> 262144 ints needed to fill the entire L2 cache
            //Another way to look at 262144 is as a multiple
            //Multiple??? What do I mean??
            //262144 * 0 = 0 , 262144 * 1 = 262144 , 262144 * 2 = 524288 , 262144 * 3 = 786432 , 262144 * 4 = 1048576
            //Through the above information we discover that:
                //1. 000000 - 262143 integers are used to completely fill the L2 cache first time
                //2. 262144 - 524287 integers are used to completely fill the L2 cache second time
                //3. 524288 - 786431 integers are used to completely fill the L2 cache third time
                //4. 786432 - 1048575 integers are used to completely fill the L2 cache fourth time
            //This same logic could be applied to "A" set of the L2 cache as well

            //Through the below loop, we are targetting only a specific set, in the L2 AND L1 cache
            //such that the cache line in L1 gets overwritten with new integers, however, still being present in L2 cache

            //16 integers * 512 sets * 8 associativity -> (00000 - 65536)65536 integers needed to completely fill the L2 cache AND
            //                                            (0 - 8191) 8192 integers needed to fill the first cache line of ALL(512) sets
            for(int num_of_iter = 0 ; num_of_iter < 4; ++num_of_iter){

                int stride_start = 65536 * num_of_iter;
                for(int stride = stride_start; stride < (stride_start + 65536); stride = stride + 8192){
                    mem_addr = &L2_buffer[stride];

                    //an instruction that loads from memory AND that precedes an LFENCE, RECEIVES data from memory PRIOR to completion of the LFENCE
                    //With the help of LFENCE, we are making sure that the data indexed by target2 is indeed present within the L1 cache
                    //The memory clobber stops the compiler from optimizing/re-ordering the instruction, this is a compiler level memory barrier
                    asm volatile("mov (%1), %0\n\t"
                                "lfence"
                                : "=&r" (data_at_mem)
                                : "r" (mem_addr)
                                : "memory");
                }

                //From Intel SDM Volume 2B:
                //The RDTSCP instruction is not a serializing instruction, but it does wait until all previous instructions have executed and all previous loads are GLOBALLY VISIBLE.
                //If software requires RDTSCP to be executed prior to execution of any subsequent instruction (including any memory accesses), it can execute LFENCE immediately after RDTSCP
                //Having the guarantee from RDTSCP that the data will be globally visible confirms that data indexed by target2 will indeed be present within the cache
                asm volatile("RDTSCP\n\t"
                            "lfence"
                            :
                            :
                            : "%rax", "%rcx", "%rdx", "memory");
            }

            //START THE TIMER
            asm volatile ("CPUID\n\t"
                        "RDTSC\n\t"
                        "mov %%edx, %0\n\t"
                        "mov %%eax, %1\n\t"
                        : "=r" (cycles_high_start), "=r" (cycles_low_start)
                        :
                        : "%rax", "%rbx", "%rcx", "%rdx");

            asm volatile("mov (%1), %0\n\t"
                        : "=&r" (data_at_mem)
                        : "r" (&L2_buffer[8192 * (data_at_mem % 8)])
                        : "memory" );

            //STOP THE TIMER
            asm volatile("RDTSCP\n\t"
                        "mov %%edx, %0\n\t"
                        "mov %%eax, %1\n\t"
                        "CPUID\n\t"
                        : "=r" (cycles_high_stop), "=r" (cycles_low_stop)
                        :
                        : "%rax", "%rbx", "%rcx", "%rdx");

            *timing = ((((uint64_t)cycles_high_stop << 32) | cycles_low_stop) - (((uint64_t)cycles_high_start << 32) | cycles_low_start));

        		time_for_hits += *timing;
            L2_hit_times[i] = *timing;
            L2_data_at_times[i] = data_at_mem;
        }

        printf("Average time for L2 cache hit: %llu cycles\n", time_for_hits / NUM_OF_TRACES);
}

void doL3Trace(){

        unsigned cycles_high_start, cycles_low_start, cycles_high_stop, cycles_low_stop;
        uint64_t time_for_hits = 0;
        volatile void* mem_addr = NULL;
        volatile void* target_L3 = &L2_buffer[8191];
        volatile int data_at_mem = -1;

	      //L3 cache hits
        for(int i=0; i < NUM_OF_TRACES; ++i){

            //logic is similar to L2 cache
            //16 integers x 49152 sets x 16 assoc = 3145728 integers
            for(int num_of_iter = 0 ; num_of_iter < 4; ++num_of_iter){

                int stride_start = 3145728 * num_of_iter;
                for(int stride = stride_start; stride < (stride_start + 3145728); stride = stride + 196608){
                    mem_addr = &L3_buffer[stride];
                    asm volatile("mov (%1), %0\n\t"
                                "lfence"
                                : "=&r" (data_at_mem)
                                : "r" (mem_addr)
                                : "memory");
                }

                asm volatile("RDTSCP\n\t"
                            "lfence"
                            :
                            :
                            : "%rax", "%rcx", "%rdx", "memory");
            }

            //START THE TIMER
            asm volatile ("CPUID\n\t"
                        "RDTSC\n\t"
                        "mov %%edx, %0\n\t"
                        "mov %%eax, %1\n\t"
                        : "=r" (cycles_high_start), "=r" (cycles_low_start)
                        :
                        : "%rax", "%rbx", "%rcx", "%rdx");

            asm volatile("mov (%1), %0\n\t"
                        : "=&r" (data_at_mem)
                        : "r" (&L3_buffer[196608 * (data_at_mem % 16)])
                        : "memory");

            //STOP THE TIMER
            asm volatile("RDTSCP\n\t"
                        "mov %%edx, %0\n\t"
                        "mov %%eax, %1\n\t"
                        "CPUID\n\t"
                        : "=r" (cycles_high_stop), "=r" (cycles_low_stop)
                        :
                        : "%rax", "%rbx", "%rcx", "%rdx");

            *timing = ((((uint64_t)cycles_high_stop << 32) | cycles_low_stop) - (((uint64_t)cycles_high_start << 32) | cycles_low_start));

        		time_for_hits += *timing;
            L3_hit_times[i] = *timing;
            L3_data_at_times[i] = data_at_mem;
        }

        printf("Average time for L3 cache hit: %llu cycles\n", time_for_hits / NUM_OF_TRACES);
}

void write_times_to_file(){
    for (int i = 0; i < NUM_OF_TRACES; i++) {
        //fprintf(cache_hit_file, "%llu\n", hit_times[i]);
        fprintf(L1_cache_hit_file, "%llu \t\t\t %d\n", L1_hit_times[i], L1_data_at_times[i]);
        fprintf(L2_cache_hit_file, "%llu \t\t\t %d\n", L2_hit_times[i], L2_data_at_times[i]);
        fprintf(L3_cache_hit_file, "%llu \t\t\t %d\n", L3_hit_times[i], L3_data_at_times[i]);
        fprintf(SDRAM_hit_file, "%llu \t\t\t %d\n", SDRAM_hit_times[i], SDRAM_data_at_times[i]);
    }
}

void init(){

    srand(time(NULL));
    for(int dataPtr = 0; dataPtr < L1_CACHE_SIZE_IN_BYTES; ++dataPtr){
        L1_buffer[dataPtr] = rand() % 32767;
    }
    for(int dataPtr = 0; dataPtr < L2_CACHE_SIZE_IN_BYTES; ++dataPtr){
        L2_buffer[dataPtr] = rand() % 262143;
    }
    for(int dataPtr = 0; dataPtr < L3_CACHE_SIZE_IN_BYTES; ++dataPtr){
        L3_buffer[dataPtr] = rand() % 12582911;
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

    printf("\n\n\nStart of L3 eviction buffer | 1st CL of SET 1 : %p\n", &L2_buffer[0]);
    printf("Start+16 of L3 eviction buffer | 1st CL of SET 2 : %p\n", &L2_buffer[16]);
    printf("Start+1008 of L3 eviction buffer | 1st CL of SET 64 : %p\n", &L2_buffer[1008]);
    printf("Start+1023 of L3 eviction buffer: %p\n", &L2_buffer[1023]);
    printf("Start+1024 of L3 eviction buffer | 2nd CL of SET 1 : %p\n", &L2_buffer[1024]);

    // setup files pointer for writing
    printf("preparing data collection\n");
    timing                    = (uint64_t*) malloc(sizeof(uint64_t));
    L1_hit_times              = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L2_hit_times              = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L3_hit_times              = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    SDRAM_hit_times           = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L1_data_at_times          = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L2_data_at_times          = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L3_data_at_times          = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    SDRAM_data_at_times       = (uint64_t*)malloc(NUM_OF_TRACES * sizeof(uint64_t));
    L1_cache_hit_file         = fopen("L1_cache_hits.txt", "w");
    L2_cache_hit_file         = fopen("L2_cache_hits.txt", "w");
    L3_cache_hit_file         = fopen("L3_cache_hits.txt", "w");
    SDRAM_hit_file            = fopen("SDRAM_cache_hits.txt", "w");
}
