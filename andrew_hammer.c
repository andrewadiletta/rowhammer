//gcc -o andrew andrew_hammer.c

#include <stdio.h>
#include <stdint.h>			// uint64_t
#include <stdlib.h>			// For malloc
#include <string.h>			// For memset
#include <time.h>
#include <fcntl.h>			// For O_RDONLY in get_physical_addr fn 
#include <unistd.h>			// For pread in get_physical_addr fn, for usleep
#include <sys/mman.h>
#include <stdbool.h>		// For bool
#include <sys/stat.h>

#define PAGE_COUNT 256 * (uint64_t)256	// ARG2 is the buffer size in MB
#define PAGE_SIZE 4096
#define PEAKS PAGE_COUNT/256*2
#define ROUNDS2 10000
#define THRESH_OUTLIER 700	// Adjust after looking at outliers in t2.txt

// Row_conflict
#define clfmeasure(_memory, _memory2, _time)\
do{\
   register uint32_t _delta;\
   asm volatile(\
   "mov %%rdx, %%r11;"\
   "clflush (%%r11);"\
   "clflush (%%rbx);"\
   "mfence;"\
   "rdtsc;"\
   "mov %%eax, %%esi;"\
   "mov (%%rbx), %%ebx;"\
   "mov (%%r11), %%edx;"\
   "rdtscp;"\
   "sub %%esi, %%eax;"\
   "mov %%eax, %%ecx;"\
   : "=c" (_delta)\
   : "b" (_memory), "d" (_memory2)\
   : "esi", "r11"\
   );\
   *(uint32_t*)(_time) = _delta;\
}while(0)

// Row_hammer for 1->0 flips
#define hammer10(_memory, _memory2)\
do{\
   asm volatile(\
   "mov $1000000, %%r11;"\
   "h10:"\
   "clflush (%%rdx);"\
   "clflush (%%rbx);"\
   "mov (%%rbx), %%r12;"\
   "mov (%%rdx), %%r13;"\
   "dec %%r11;"\
   "jnz h10;"\
   : \
   : "b" (_memory), "d" (_memory2)\
   : "r11", "r12", "r13"\
   );\
}while(0)

struct continuous_memory{
	uint8_t ** memory_addresses;
	int length;
	int start;
	int end;
};

struct continuous_bank{
	int *conflict;
	int indices;
};

// Taken from https://github.com/IAIK/flipfloyd
static uint64_t get_physical_addr(uint64_t virtual_addr)
{
	static int g_pagemap_fd = -1;
	uint64_t value;

	// open the pagemap
	if(g_pagemap_fd == -1) {
	  g_pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	}
	if(g_pagemap_fd == -1) return 0;

	// read physical address
	off_t offset = (virtual_addr / 4096) * sizeof(value);
	int got = pread(g_pagemap_fd, &value, sizeof(value), offset);
	if(got != 8) return 0;

	// Check the "page present" flag.
	if(!(value & (1ULL << 63))) return 0;

	// return physical address
	uint64_t frame_num = value & ((1ULL << 55) - 1);
	return (frame_num * 4096) | (virtual_addr & (4095));
}

//return array containing start and end of a physically contiguous memory region
void get_continuous_mem(struct continuous_memory * ret, uint8_t * buffer){
	uint64_t phys_addr[PAGE_COUNT] = {0};
	uint64_t virt_addr[PAGE_COUNT] = {0};

	//Create array of physical addresses
	for (int k = 0; k < PAGE_COUNT; k++)
	{
		int phys = get_physical_addr((uint64_t) &buffer[k*PAGE_SIZE]);
		int virt = (uint64_t) &buffer[k*PAGE_SIZE];
		phys_addr[k] = phys>>12;
		virt_addr[k] = virt>>12;
    }
	
	int continuous = 0;
	int past_addr  = 0;
	int max = -1;
	int start = 0;
	int end = 0;
	int starttmp = 0;
	
	for(int p = 0; p < PAGE_COUNT; p++){
		if(phys_addr[p] == past_addr+1){
			continuous++;
			if(continuous > max){
				max = continuous;
			}	
		}
		else{
			if(continuous >= max){
				end = p-1;
				start = starttmp;
			}
			starttmp = p;
			continuous = 0;
		}
		past_addr = phys_addr[p];
	}

	//Create an array of pointers to the memory addresses that are continuous
	uint8_t * special_buffer[end-start];
	ret->memory_addresses = malloc(sizeof(uint8_t*) * (end-start));
	for(int i = start; i < end; i++){
		special_buffer[i-start] = &buffer[i*PAGE_SIZE];
	}
	ret->memory_addresses = special_buffer;
	ret->length = end-start;
	ret->start = start;
	ret->end = end;
}

//Method will return continuous memory going into the same bank
void getContinuousBank(struct continuous_bank* return_bank, struct continuous_memory * continuous_buffer, uint8_t * buffer){
	return_bank->conflict = (int*)malloc(sizeof(int)*(continuous_buffer->length));
	clock_t cl = clock();
	//Running row_conflict on the detected contiguous memory to find addressess going into the same bank
	#define THRESH_ROW_CONFLICT 350	// Adjust after looking at c.txt
	int conflict[PEAKS] = {0};
	int conflict_index = 0;
	int total;

	uint32_t tt = 0;
	float timer = 0.0;

	printf("Testing a total of %d addresses\n", continuous_buffer->length);

	for (int p = continuous_buffer->start; p < continuous_buffer->end; p++)
	{
		total = 0;
		int cc = 0;
		for (int r = 0; r < ROUNDS2; r++)
		{			
			clfmeasure(&buffer[continuous_buffer->start*PAGE_SIZE], &buffer[p*PAGE_SIZE], &tt);
			if (tt < THRESH_OUTLIER)
			{
				total = total + tt;
				cc++;
			}
		}
		//return_bank->conflict[p-continuous_buffer->start] = total / cc;
		if (total/cc > THRESH_ROW_CONFLICT)
		{
			return_bank->conflict[conflict_index] = p;
			conflict_index++;
		}
	}
	cl = clock() - cl;
	timer = ((float) cl)/CLOCKS_PER_SEC;

	//print out the conflict_index
	printf("Number of indicies in conflict: %d\n", conflict_index);
	return_bank->indices = conflict_index;
}

//Function that finds flips in the memory
void get_flips(uint8_t *myBuffer, struct continuous_bank * myBank){
	clock_t cl;
	cl = clock();
	///////////////////////////////DOUBLE_SIDED_ROWHAMMER//////////////////////
	#define MARGIN 1			// Margin is used to skip initial rows to get flips earlier
								// Reason: In the start memory is not very contiguous
	#ifdef PRINTING
		printf("\n------------DOUBLE SIDED HAMMERING on DETECTED CONTIGUOUS MEMORY-------------\n\n");
	#endif
	
	int h;
	bool flip_found10 = false;
	bool flip_found01 = false;
	//int flippy_addr_count10 = 0;
	int flippy_addr_count01 = 0;
	int flips_per_row10 = 0;
	int flips_per_row01 = 0;
	int total_flips_10 = 0;
	int total_flips_01 = 0;
	uint64_t flippy_virt_addr10 = 0;
	uint64_t flippy_virt_addr01 = 0;
	uint64_t flippy_phys_addr10 = 0;
	uint64_t flippy_phys_addr01 = 0;
	int flippy_offsets10[PAGE_SIZE] = {0};
	int flippy_offsets01[PAGE_SIZE] = {0};
	

	
	for (h = MARGIN; h < myBank->indices - 2; h=h+2)
	{
		
		//For 1->0 FLIPS
		printf("Hammering Rows %i %i %i\n", h/2, h/2+1, h/2+2);
		
		// Filling the Victim and Neighboring Rows with Own Data
		// Not getting flips if victim initialized with 0x00
		
		for (int y = 0; y < PAGE_SIZE; y++)
		{
			myBuffer[(myBank->conflict[h]*PAGE_SIZE)+y] = 0x00;	// Top Row
			myBuffer[(myBank->conflict[h+2]*PAGE_SIZE)+y] = 0xFF;	// Victim Row
			myBuffer[(myBank->conflict[h+4]*PAGE_SIZE)+y] = 0x00;	// Bottom Row
			//printf("%02x", evictionBuffer[(conflict[2]*PAGE_SIZE)+y]);
		}

		// Hammering Neighboring Rows
		hammer10(&myBuffer[myBank->conflict[h]*PAGE_SIZE], &myBuffer[myBank->conflict[h+4]*PAGE_SIZE]);

		// Checking for Bit Flips
		
		flips_per_row10 = 0;
		int total_flips10 = 0;
		for (int y = 0; y < PAGE_SIZE; y++)
		{
			if (myBuffer[(myBank->conflict[h+2]*PAGE_SIZE)+y] != 0xFF)
			{
				flip_found10 = true;
				printf("%lx 1->0 FLIP at page offset %03x\tvalue %02x\n", get_physical_addr((uint64_t)&myBuffer[(myBank->conflict[h+2]*PAGE_SIZE)]), y, myBuffer[(myBank->conflict[h+2]*PAGE_SIZE)+y]);
				flippy_offsets10[flips_per_row10] = y;
				flips_per_row10++;
				total_flips10++;
			}
		}
	}
}

int main(int argc, char * argv[])
{
    printf("Hello World!\n");
	system("echo 1 | sudo tee /proc/sys/vm/compact_memory");
	
	srand((unsigned int) time(NULL));
	//Create buffer array
	uint8_t * search_buffer = mmap(NULL, PAGE_COUNT * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	struct continuous_memory * continuous_memory = malloc(sizeof(struct continuous_memory));

	get_continuous_mem(continuous_memory, search_buffer);
	printf("Got continuous memory\n");

	printf("getting bank\n");
	struct continuous_bank *continuous_bank = malloc(sizeof(struct continuous_bank));
	getContinuousBank(continuous_bank, continuous_memory, search_buffer);
	printf("got bank\n");

	//Return the first integer in bank
	uint8_t number_of_indicies = continuous_bank->indices;
	printf("Bank from main code: %d\n", continuous_bank->indices);


	get_flips(search_buffer, continuous_bank);

    return 1;
}
