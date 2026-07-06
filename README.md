# Process Scheduler Simulator

A graphical **CPU Process Scheduling Simulator** developed in **C** using **Raylib**. The project allows users to configure processes, compare multiple scheduling algorithms, and visualize their execution in real time through CPU status, process statistics, and a Gantt chart.

## Features

- Interactive graphical interface built with Raylib
- Process configuration menu
- Persistent configuration (saved between executions)
- Real-time simulation
- Live Gantt chart
- CPU status visualization
- Process statistics
- Average Turnaround Time and Waiting Time
- Configurable context switch overhead

## Implemented Scheduling Algorithms

### 1. FIFO (First-In First-Out)

- Non-preemptive
- Executes processes in arrival order.
- Once a process starts, it runs until completion.

---

### 2. Round Robin (RR)

- Preemptive
- Uses a configurable time quantum.
- When the quantum expires, the scheduler selects the next ready process.
- Includes configurable context switch overhead.

---

### 3. Earliest Deadline First (EDF)

- Preemptive
- Always executes the process with the earliest deadline.
- Supports:
  - Preemption when a more urgent process arrives.
  - Quantum-based rescheduling.
- Includes configurable context switch overhead.

---

### 4. CTS (Completely Fair Scheduler-inspired)

Inspired by Linux's Completely Fair Scheduler (CFS).

- Preemptive
- Uses a virtual runtime (`vruntime`) for each process.
- The process with the smallest `vruntime` is selected.
- Context switches are considered instantaneous in this implementation.

---

### 5. MOB (Multi-Objective Scheduler)

Custom scheduling algorithm developed for this project.

The scheduler computes a score for every ready process using multiple normalized metrics:

```
Score =
    w1 × Waiting Time
  + w2 × Deadline Urgency
  + w3 × Burst Urgency
  + w4 × Priority
  - w5 × Switch Penalty
```

Where:

- **Waiting Time** favors long-waiting processes.
- **Deadline Urgency** prioritizes processes approaching their deadlines.
- **Burst Urgency** favors shorter remaining execution times.
- **Priority** is a user-defined value.
- **Switch Penalty** discourages unnecessary context switches.

The scheduler dynamically recalculates the score every frame and always selects the process with the highest score.

Although inspired by utility-based decision systems, MOB does **not** use machine learning.

---

## Context Switch Simulation

The simulator models context switch overhead explicitly.

Whenever a scheduling algorithm decides to replace the currently running process:

1. CPU execution stops.
2. A configurable context switch timer starts.
3. No process executes during this interval.
4. After the overhead expires, execution resumes with the selected process.

The Gantt chart displays context switch periods in **orange**.

## Gantt Chart

The simulator records every CPU state transition:

- Running process
- CPU idle
- Context switch overhead

This allows real-time visualization of scheduling behavior.

## Process Statistics

Each process tracks:

- Arrival Time
- Burst Time
- Remaining Time
- Deadline
- Priority
- Completion Time
- Turnaround Time
- Waiting Time

The simulator also computes:

- Average Turnaround Time
- Average Waiting Time

## Configuration

The simulator automatically saves its configuration to:

```
scheduler_config.txt
```

Saved settings include:

- Number of processes
- Quantum
- Context switch overhead
- MOB weights
- Process parameters

These settings are automatically loaded on the next execution.

## Controls

### Menu

- Configure processes
- Configure quantum
- Configure context switch overhead
- Configure MOB weights
- Save configuration
- Launch simulation

### Simulation

| Key | Action |
|------|--------|
| **1** | FIFO |
| **2** | Round Robin |
| **3** | EDF |
| **4** | CTS |
| **5** | MOB |
| **Esc** | Return to configuration menu |

## Technologies

- C
- Raylib

## Project Structure

```
.
├── main.c
├── scheduler_config.txt
└── README.md
```

## Compilation

Example using GCC:

```bash
gcc main.c -o scheduler \
    -lraylib \
    -lopengl32 \
    -lgdi32 \
    -lwinmm
```

For Linux:

```bash
gcc main.c -o scheduler \
    -lraylib \
    -lm \
    -lpthread \
    -ldl \
    -lrt \
    -lX11
```

## Educational Purpose

This project was developed for educational purposes to demonstrate and compare classical CPU scheduling algorithms while introducing a custom multi-objective scheduling strategy that combines several scheduling criteria into a single decision function.

## Authors

Bernardo Soares Santos

Developed as part of the **Data Structures and Algorithms II** course.
