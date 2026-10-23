# RIOT demo and test tools

Demonstration and bench tools around the RIOT controller. The production ESP32-P4 controller firmware is developed separately.

| Folder | What it is |
|---|---|
| [`trmnl/`](trmnl/) | Riot-Flow demo controller on the Seeed XIAO ESP32-S3 with the 7.5" TRMNL ePaper display (the first Riot-Flow controller). |
| [`cyd-tester/`](cyd-tester/) | CYD-TESTER: a Modbus TCP test unit for RIOT controllers on an ESP32-2432S028R "Cheap Yellow Display". Brought in with its history from the former CYD-TESTER repo. |
| [`controller-introduction/`](controller-introduction/) | Browser-based RIOT controller introduction and training simulator with scenario files (Python, no hardware needed). |
| [`archive/io-tester-2432s028/`](archive/io-tester-2432s028/) | The earlier 2432S028 I/O tester that CYD-TESTER replaced. Kept for reference. |

Each folder has its own README with build and run instructions.
