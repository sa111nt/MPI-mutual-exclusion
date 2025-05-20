# MPI-mutual-exclusion
## Distributed Troll Slayer: MPI + Lamport Mutual Exclusion

Each process represents a troll hunter, and each city is an independent critical resource. Processes can independently request access to a city, but only one process may be inside a city at a time.
To enter a city, a process sends a REQ message with the city ID, its Lamport clock, and process ID. Other processes compare priorities based on timestamps and grant access (ACK) accordingly. After leaving the city, the process sends ACKs to those who had lower priority requests, allowing them to enter.

### Compilation and execution
```bash
mpic++ troll-slayer.cpp -o troll-slayer
mpirun -np 4 ./troll-slayer > output
