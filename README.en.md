# Hashi-DB

Hashi-DB is a high-performance in-memory database system developed using C++. It adopts a modular design, focusing on providing efficient cache management and data operation capabilities.

## Project Features

- **Type-Safe Data Operations**: Supports storage and manipulation of INT, LONG, DOUBLE, STRING, and custom data types.
- **Flexible CRUD Architecture**: Lightweight operation layer based on reflection tables and arrays of function pointers.
- **Memory Management Optimization**: Smart memory allocation strategies and data expiration cleanup mechanisms.
- **Command-Based Design**: Unified cache and disk I/O command processing framework.
- **Client-Server Architecture**: Supports distributed deployment with network communication.

## Core Modules

```
hashi-db/
├── main.cpp                 # Program entry point
├── CMakeLists.txt           # Build configuration
└── project/
    └── data_memo_level/
        └── memory/
            └── data_opt/    # Data operation layer (CRUD)
```

## Data Type Support

| Type Identifier | Description |
|-----------------|-------------|
| `INT_OPT` | Integer type |
| `LONG_OPT` | Long integer type |
| `DOUBLE_OPT` | Double precision floating point |
| `STRING_OPT` | String type |
| `CUSTOM_OPT` | Custom type |

## Operation Interfaces

| Operation Type | Functionality Description |
|----------------|---------------------------|
| `INSERT` | Insert new data |
| `DELETE` | Delete data |
| `MODIFY` | Modify data |
| `SELECT` | Query data |

## Build Guide

```bash
# Create build directory
mkdir build && cd build

# Generate Makefile
cmake ..

# Compile project
make
```

## Project Documentation

Detailed design documents are located in the `doc/` directory:

- **Cache Design**: `doc/plan/exec_level/cache_exe.md`
- **Disk Operations**: `doc/plan/exec_level/disk_exe.md`
- **Memory Management**: `doc/plan/memo_manage_level/`
  - Allocation Strategy: `memo_assign.md`
  - Cleanup Strategy: `memo_clean.md`
- **Command Design**: `doc/plan/command_map_level/`
- **Network Communication**: `doc/plan/net_level/` (Server/Client)
- **Log System**: `doc/plan/logs_level/log.md`

## Development Log

Project development progress is recorded in the `doc/log/` directory, including daily development goals, implementation details, and issue logs.

## License

This project follows open-source protocols. For specific license information, please refer to the project repository.