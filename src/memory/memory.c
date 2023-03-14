#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>

#include "../int.h"
#include "specialAddrs.h"
#include "../cpu.h"
#include "../interrupts.h"
#include "../hardware/display/display.h"

extern CpuState cpu;

// u8 memory[0x88023F98] = {0};
// u8* memory = NULL;

// Page table
// The first half is virtual memory -> host memory
// The second half is physical memory -> host memory
// TODO: Proper MMU emulation
void* pageTable[0x100000] = {0};

void createMemArea(void* hostMem, u32 start, u32 end) {
  for (u32 i = start; i < end; i += 0x1000) {
    pageTable[i / 0x1000] = hostMem + (i - start);
  }
}

void* allocMemArea(u32 start, u32 end) {
  void* hostMem = calloc(end - start, 1);
  createMemArea(hostMem, start, end);
  return hostMem;
}

void createAlias(u32 start, u32 end, u32 alias) {
  for (u32 i = start; i < end; i += 0x1000) {
    pageTable[alias / 0x1000 + (i - start) / 0x1000] = pageTable[i / 0x1000];
  }
}

void initMemory(void) {
  // memory = calloc(0xffffffff, sizeof(u8));

  FILE *f = fopen("/home/heath/calc-emu-2/calc-emu-test/MyAddin.g3a", "rb");
  if (f == NULL) {
    printf("Error opening file\n");
    exit(1);
  }

  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  // Load from 0x7000
  fseek(f, 0x7000, SEEK_SET);

  // Allocate a memory area at 0x80000000 (ROM) to store the file
  // The file will be laoded at an offset of 0x20000 to make space for the product ID at 0x8001ffd0
  void* hostMem = allocMemArea(0x80000000, 0x80000000 + fsize + 0x20000);
  fread(hostMem + 0x20000, fsize, 1, f);
  // Swap all the 4-byte longwords
  for (int i = 0; i < fsize; i += 4) {
    u32* word = (u32*) (hostMem + 0x20000 + i);
    *word = __bswap_32(*word);
  }
  fclose(f);
  // Alias at 0xa0000000 (P2)
  createAlias(0x80000000, 0x80000000 + fsize, 0xa0000000);
  // Set up an alias at 0x00300000 in virtual memory to the file
  createAlias(0x80020000, 0x80020000 + fsize, 0x00300000);

  // TODO: set the product ID? Zeroing it out should be fine for now
  // We allocated with calloc so it's already zeroed out

  // Allocate an 8MB memory area at 0x8c000000 (RAM)
  allocMemArea(0x8c000000, 0x8c000000 + (8 * 1024 * 1024));
  // Alias at 0xac000000 (P2)
  createAlias(0x8c000000, 0x8c000000 + (8 * 1024 * 1024), 0xac000000);
  // 512KB aliased to virtual memory at 0x08100000
  createAlias(0x8c000000, 0x8c000000 + (512 * 1024), 0x08100000);

  // 4KB of ILRAM at e5200000
  allocMemArea(0xe5200000, 0xe5200000 + 0x1000);
}

// Some ways of handling the endianness difference are described here:
// https://skmp.dev/blog/negative-addressing-bswap/
// Here we use the "XOR trick", because I'm not convinced that negative
// addressing is faster but I might need to test it

u32 readMemory(u32 address, u32 size) {
  // Check for unaligned access
  if (address % size != 0) {
    printf("Unaligned memory access at %08x, PC = %08x\n", address, cpu.reg.PC);
    exit(1);
  }

  // Get the relevant page
  void* page = pageTable[address / 0x1000];

  // Check if the page is valid
  u32 value;
  if (page == NULL) {
    // It could be a special address
    // if (readSpecial(address, &value, size)) {
    //   // Shift and mask the value to simulate a read of the correct size
    //   if (size != 4) {
    //     value >>= (address % 4) * 8;
    //     value &= (1 << (size * 8)) - 1;
    //   }
    //   return value;
    // }

    if (readSpecial(address, size, &value)) {
      return value;
    }

    printf("Invalid memory read at %08x, PC = %08x\n", address, cpu.reg.PC);
    // exit(1);
    cpu.reg.TEA = address;
    raiseInterrupt(0x040, cpu.reg.VBR + 0x100);
    return 0;
  }

  switch (size) {
    case 1:
      // xor the address with 3
      value = *(u8*)(page + ((address ^ 0x3) % 0x1000));
      break;
    case 2:
      // xor the address with 2
      value = *(u16*)(page + ((address ^ 0x2) % 0x1000));
      break;
    case 4:
      value = *(u32*)(page + (address % 0x1000));
      break;
    default:
      printf("Invalid memory read size: %d\n", size);
      exit(1);
  }

  return value;
}

void writeMemory(u32 address, u32 size, u32 value) {
  // Check for unaligned access
  if (address % size != 0) {
    printf("Unaligned memory access at %08x\n", address);
    exit(1);
  }

  // Get the relevant page
  void* page = pageTable[address / 0x1000];

  // Check if the page is valid
  if (page == NULL) {
    // It could be a special address
    // if (address == 0xb4000000 && displayFastPath && size == 2) {
    //   fastWrite(value);
    //   return;
    // }

    // // Work out the bitmask used to select what bits to write
    // u32 mask;
    // if (size == 4) {
    //   mask = 0xffffffff;
    // } else {
    //   mask = (1 << (size * 8)) - 1;
    // }
    // // Shift the mask to the correct position
    // mask <<= (address % 4) * 8;
    // // Shift the value to the correct position
    // value <<= (address % 4) * 8;
    // if (writeSpecial(address, value, size, mask)) {
    //   return;
    // }

    if (writeSpecial(address, size, value)) {
      return;
    }

    printf("Invalid memory write at %08x, PC = %08x\n", address, cpu.reg.PC);
    cpu.reg.TEA = address;
    raiseInterrupt(0x040, cpu.reg.VBR + 0x100);
    // exit(1);
    return;
  }

  switch (size) {
    case 1:
      // xor the address with 3
      *(u8*)(page + ((address ^ 0x3) % 0x1000)) = value;
      break;
    case 2:
      // xor the address with 2
      *(u16*)(page + ((address ^ 0x2) % 0x1000)) = value;
      break;
    case 4:
      *(u32*)(page + (address % 0x1000)) = value;
      break;
    default:
      printf("Invalid memory write size: %d\n", size);
      exit(1);
  }
}
