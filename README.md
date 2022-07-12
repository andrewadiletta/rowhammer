# What is this repo?

This repo contains code exploring an implementation of the Rowhammer attack on a Unix server using DDR3 memory to access normally in-accessible memory through a vulnerability in the physical hardware that exploits the tendency of cells in DRAM to experience voltage leakage which leads to data errors. The setup for the attack, and organizing virtual memory to align to physical memory, and attacking cells housed in the same bank will be explored using side channels. These side channel attacks will focus on finding memory that appears consecutively in both physical and virtual memory, and aligning a victim row between two attacking rows, and reading from the attacking row at a high rate of speed in order to flip bits in the victim row.

