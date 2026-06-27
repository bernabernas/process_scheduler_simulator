#include "raylib.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAX_PROCESSES 10
#define MAX_GANTT_ENTRIES 4000
#define CONFIG_FILENAME "scheduler_config.txt"

typedef enum {
    FIFO,
    RR,
    EDF,
    CTS
} SchedulerType;

// We need a state machine to separate the Home Menu from the Simulation
typedef enum {
    STATE_MENU,
    STATE_RUNNING
} AppState;

typedef struct {
    int id;
    float arrival_time;
    float burst_time;
    float remaining_time;
    float deadline;
    float vruntime;
    Color color;

    // --- Stats tracking ---
    bool finished;
    float completion_time;
} Process;

// --- Gantt chart history entry ---
// pid >= 0 : process running
// pid == -1: CPU idle (no ready process)
// pid == -2: CPU busy performing a context switch (overhead)
typedef struct {
    int pid;
    float start;
    float end;
    Color color;
} GanttEntry;

// --- Configurable Variables (Modified via Home Page) ---
int process_count = 5;
float time_slice = 1.0f; // Global quantum variable
Process processes[MAX_PROCESSES];

float current_time = 0;
int current_process = -1;
static int rrIndex = -1;
SchedulerType scheduler = FIFO;
float rr_timer = 0;
AppState current_state = STATE_MENU;
float cts_slice = 1.0f; // Global slice for CTS scheduler
float cts_timer = 0;

// Tracks which property string a user is currently typing into
int active_input_field = -1;
char input_buffer[16] = { 0 };

// Gantt chart history
GanttEntry ganttChart[MAX_GANTT_ENTRIES];
int ganttCount = 0;

// Small on-screen feedback message (e.g. "Configuration saved!")
char status_message[64] = { 0 };
float status_message_timer = 0;

// --- Context switch overhead (RR and EDF only) ---
float context_switch_time = 0.3f;   // duration of the overhead, in seconds
float context_timer = 0.0f;
bool in_context_switch = false;
int next_process = -1;

void InitProcessesDefaults() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].id = i;
        processes[i].arrival_time = i * 1.5f;
        processes[i].burst_time = 3.0f + (i % 3) * 2.0f;
        processes[i].remaining_time = processes[i].burst_time;
        processes[i].deadline = processes[i].arrival_time + 10.0f;
        processes[i].vruntime = 0;
        processes[i].finished = false;
        processes[i].completion_time = 0;
        processes[i].color = (Color){
            (unsigned char)(50 + (i * 40) % 205),
            (unsigned char)(80 + (i * 60) % 175),
            (unsigned char)(100 + (i * 30) % 155),
            255
        };
    }
}

//###############################################################################################################
//#################### Config persistence (saves/loads input values to disk) ####################################
//###############################################################################################################

bool SaveConfig() {
    FILE* f = fopen(CONFIG_FILENAME, "w");
    if (!f) return false;

    fprintf(f, "%d %.3f %.3f\n", process_count, time_slice, context_switch_time);
    for (int i = 0; i < process_count; i++) {
        fprintf(f, "%.3f %.3f %.3f\n",
                processes[i].arrival_time,
                processes[i].burst_time,
                processes[i].deadline);
    }
    fclose(f);
    return true;
}

// Fills slot i with a generated default (used to pad unused slots after loading)
static void FillDefaultProcess(int i) {
    processes[i].id = i;
    processes[i].arrival_time = i * 1.5f;
    processes[i].burst_time = 3.0f + (i % 3) * 2.0f;
    processes[i].remaining_time = processes[i].burst_time;
    processes[i].deadline = processes[i].arrival_time + 10.0f;
    processes[i].vruntime = 0;
    processes[i].finished = false;
    processes[i].completion_time = 0;
    processes[i].color = (Color){
        (unsigned char)(50 + (i * 40) % 205),
        (unsigned char)(80 + (i * 60) % 175),
        (unsigned char)(100 + (i * 30) % 155),
        255
    };
}

// Returns true if a valid config file was found and loaded
bool LoadConfig() {
    FILE* f = fopen(CONFIG_FILENAME, "r");
    if (!f) return false;

    int pc;
    float ts, cst;
    if (fscanf(f, "%d %f %f", &pc, &ts, &cst) != 3 || pc < 1 || pc > MAX_PROCESSES) {
        fclose(f);
        return false;
    }

    process_count = pc;
    time_slice = ts;
    context_switch_time = cst;
    if (context_switch_time < 0.0f) context_switch_time = 0.0f;

    for (int i = 0; i < process_count; i++) {
        float a, b, d;
        if (fscanf(f, "%f %f %f", &a, &b, &d) != 3) {
            fclose(f);
            InitProcessesDefaults();
            return false;
        }
        processes[i].id = i;
        processes[i].arrival_time = a;
        processes[i].burst_time = b;
        processes[i].remaining_time = b;
        processes[i].deadline = d;
        processes[i].vruntime = 0;
        processes[i].finished = false;
        processes[i].completion_time = 0;
        processes[i].color = (Color){
            (unsigned char)(50 + (i * 40) % 205),
            (unsigned char)(80 + (i * 60) % 175),
            (unsigned char)(100 + (i * 30) % 155),
            255
        };
    }

    // Pad any remaining slots (in case the user later increases process_count)
    for (int i = process_count; i < MAX_PROCESSES; i++) {
        FillDefaultProcess(i);
    }

    fclose(f);
    return true;
}

// Reset functions for transitioning from Menu -> Simulation safely
void StartSimulation() {
    current_time = 0;
    current_process = -1;
    rr_timer = 0;
    cts_timer = 0;
    ganttCount = 0;
    rrIndex = -1;

    in_context_switch = false;
    context_timer = 0;
    next_process = -1;

    for (int i = 0; i < process_count; i++) {
        processes[i].remaining_time = processes[i].burst_time;
        processes[i].vruntime = 0;
        processes[i].finished = false;
        processes[i].completion_time = 0;
    }

    // Persist the values used for this run so they're available next launch too
    SaveConfig();

    current_state = STATE_RUNNING;
}

int SelectFIFO() {
    for (int i = 0; i < process_count; i++) {
        if (processes[i].arrival_time <= current_time && processes[i].remaining_time > 0) {
            return i;
        }
    }
    return -1;
}

int SelectRR()
{
    for (int i = 0; i < process_count; i++)
    {
        rrIndex = (rrIndex + 1) % process_count;

        if (processes[rrIndex].arrival_time <= current_time &&
            processes[rrIndex].remaining_time > 0)
        {
            return rrIndex;
        }
    }

    return -1;
}

int SelectEDF() {
    int best = -1;
    float min_deadline = 1e9;

    for (int i = 0; i < process_count; i++) {
        if (processes[i].arrival_time <= current_time &&
            processes[i].remaining_time > 0 &&
            processes[i].deadline < min_deadline) {
            min_deadline = processes[i].deadline;
            best = i;
        }
    }
    return best;
}

int SelectCTS() {
    int best = -1;
    float min_vruntime = 1e9;

    for (int i = 0; i < process_count; i++) {
        if (processes[i].arrival_time <= current_time &&
            processes[i].remaining_time > 0 &&
            processes[i].vruntime < min_vruntime) {
            min_vruntime = processes[i].vruntime;
            best = i;
        }
    }
    return best;
}

int SelectProcess() {
    switch (scheduler) {
        case FIFO: return SelectFIFO();
        case RR:   return SelectRR();
        case EDF:  return SelectEDF();
        case CTS:  return SelectCTS();
    }
    return -1;
}

bool AllProcessesFinished() {
    for (int i = 0; i < process_count; i++) {
        if (!processes[i].finished) return false;
    }
    return true;
}

// Appends/extends the Gantt history so the chart can be replayed with motion
static void LogGanttSlice(float sliceStart, float sliceEnd, int pid) {
    if (ganttCount > 0 && ganttChart[ganttCount - 1].pid == pid) {
        // Same process/idle/switch state as before: just extend the last block
        ganttChart[ganttCount - 1].end = sliceEnd;
        return;
    }
    if (ganttCount >= MAX_GANTT_ENTRIES) return; // history full, stop recording

    Color c;
    if (pid == -1) c = (Color){ 60, 60, 60, 255 };        // idle, nothing ready
    else if (pid == -2) c = (Color){ 235, 150, 40, 255 };  // context switch overhead
    else c = processes[pid].color;

    ganttChart[ganttCount].pid = pid;
    ganttChart[ganttCount].start = sliceStart;
    ganttChart[ganttCount].end = sliceEnd;
    ganttChart[ganttCount].color = c;
    ganttCount++;
}

// Begins a context-switch overhead period. The CPU stays idle (running nothing)
// for context_switch_time seconds, after which `target` becomes current_process.
// Used by RR (every quantum expiry) and EDF (every new arrival), even when the
// process chosen after the switch turns out to be the very same one as before.
static void StartContextSwitch(int target) {
    in_context_switch = true;
    next_process = target;
    context_timer = 0;
    current_process = -1;
}

void UpdateScheduler(float dt) {

    // --- Currently performing a context switch: CPU does no useful work ---
    if (in_context_switch) {
        context_timer += dt;
        current_time += dt;

        LogGanttSlice(current_time - dt, current_time, -2);

        if (context_timer >= context_switch_time) {
            current_process = next_process;
            next_process = -1;
            context_timer = 0;
            in_context_switch = false;

            // The resumed process gets a fresh slice/quantum after the switch
            if (scheduler == RR) rr_timer = 0;
            if (scheduler == CTS) cts_timer = 0;
        }
        return;
    }

    if (AllProcessesFinished()) {
        current_process = -1;
        return;
    }

    float old_time = current_time;
    current_time += dt;

    int previous_process = current_process; // who was actually executing up to this instant
    bool switching = false;
    int switchTarget = -1;

    if (scheduler == RR) {
        rr_timer += dt;

        if (current_process == -1) {
            // Nothing was running (simulation start OR the previous process just
            // finished naturally) -> pick directly, no overhead.
            current_process = SelectRR();
            rr_timer = 0;
        }
        else if (rr_timer >= time_slice) {
            // Quantum expired while a process was actively running: this ALWAYS
            // triggers a context switch with overhead, even if the scheduler
            // ends up picking the very same process again.
            int candidate = SelectRR();
            rr_timer = 0;
            if (candidate != -1) {
                switching = true;
                switchTarget = candidate;
            }
        }
    }
    else if (scheduler == EDF) {
        // EDF is event-driven: it only needs to re-evaluate when a new process
        // arrives (deadlines never change while running). Detect arrivals that
        // happened exactly during this frame.
        bool arrivalEvent = false;
        for (int i = 0; i < process_count; i++) {
            if (processes[i].arrival_time > old_time && processes[i].arrival_time <= current_time) {
                arrivalEvent = true;
                break;
            }
        }

        if (current_process == -1) {
            // Nothing running (start OR previous process just finished) -> direct pick, no overhead.
            current_process = SelectEDF();
        }
        else if (arrivalEvent) {
            // A new process arrived while something was running: always pay the
            // context switch overhead to re-evaluate, even if EDF decides to keep
            // running the same process.
            int candidate = SelectEDF();
            if (candidate != -1) {
                switching = true;
                switchTarget = candidate;
            }
        }
    }
    else if (scheduler == CTS) {
        cts_timer += dt;
        if (current_process == -1 || cts_timer >= cts_slice) {
            current_process = SelectCTS();
            cts_timer = 0;
        }
    }
    else { // FIFO
        current_process = SelectProcess();
    }

    if (switching) {
        // The process that was running gets credit for the work done during this
        // very frame, right up until the switch decision boundary.
        if (previous_process != -1) {
            processes[previous_process].remaining_time -= dt;

            if (processes[previous_process].remaining_time <= 0) {
                // Edge case: the process finished at the exact same instant the
                // quantum expired / a new process arrived. A natural completion
                // NEVER incurs overhead, so we cancel the switch entirely.
                processes[previous_process].remaining_time = 0;
                processes[previous_process].finished = true;
                processes[previous_process].completion_time = current_time;
                current_process = -1; // picked up fresh next frame, no overhead
                LogGanttSlice(old_time, current_time, previous_process);
                return;
            }
        }

        LogGanttSlice(old_time, current_time, previous_process);
        StartContextSwitch(switchTarget);
        return;
    }

    // No switch this frame: log and run current_process (covers FIFO, CTS, and
    // the RR/EDF "freshly selected" or "continuing uninterrupted" cases).
    LogGanttSlice(old_time, current_time, current_process);

    if (current_process != -1) {
        processes[current_process].remaining_time -= dt;

        if (scheduler == CTS) {
            processes[current_process].vruntime += dt;
        }

        if (processes[current_process].remaining_time <= 0) {
            processes[current_process].remaining_time = 0;
            processes[current_process].finished = true;
            processes[current_process].completion_time = current_time;
            current_process = -1; // Force re-evaluation next frame, no overhead for natural completion
        }
    }
}

//###############################################################################################################
//#################### Drawing functions for the UI components and simulation visualization ######################
//###############################################################################################################

// Helper UI function to render a custom text button component
bool DrawButton(Rectangle bounds, const char* text, Color baseColor) {
    Vector2 mousePos = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mousePos, bounds);
    bool clicked = hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    DrawRectangleRec(bounds, hovered ? ColorAlpha(baseColor, 0.8f) : baseColor);
    DrawRectangleLinesEx(bounds, 1, WHITE);

    int fontSize = 16;
    int textWidth = MeasureText(text, fontSize);
    DrawText(text, bounds.x + (bounds.width - textWidth) / 2, bounds.y + (bounds.height - fontSize) / 2, fontSize, WHITE);

    return clicked;
}

// Renders individual input cells for arrival, burst, and deadline configurations
void DrawNumericInput(Rectangle rect, float* value, int fieldId) {
    Vector2 mousePos = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mousePos, rect);

    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        active_input_field = fieldId;
        snprintf(input_buffer, sizeof(input_buffer), "%.1f", *value);
    }

    Color boxColor = (active_input_field == fieldId) ? MAROON : (hovered ? DARKGRAY : BLACK);
    DrawRectangleRec(rect, boxColor);
    DrawRectangleLinesEx(rect, 1, GRAY);

    if (active_input_field == fieldId) {
        // Handle physical keyboard input strings
        int key = GetCharPressed();
        while (key > 0) {
            if (((key >= '0') && (key <= '9')) || key == '.') {
                int len = strlen(input_buffer);
                if (len < (int)sizeof(input_buffer) - 1) {
                    input_buffer[len] = (char)key;
                    input_buffer[len + 1] = '\0';
                }
            }
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int len = strlen(input_buffer);
            if (len > 0) input_buffer[len - 1] = '\0';
        }
        if (IsKeyPressed(KEY_ENTER)) {
            *value = strtof(input_buffer, NULL);
            active_input_field = -1;
            SaveConfig(); // persist as soon as the user commits a value
            strcpy(status_message, "Configuration saved!");
            status_message_timer = 1.5f;
        }
        DrawText(TextFormat("%s|", input_buffer), rect.x + 5, rect.y + 5, 16, YELLOW);
    } else {
        DrawText(TextFormat("%.1f", *value), rect.x + 5, rect.y + 5, 16, WHITE);
    }
}

void DrawMenuPage() {
    DrawText("Process Scheduler Configurator", 40, 20, 26, LIGHTGRAY);

    // Global Parameter controls
    DrawText("System Settings:", 40, 65, 18, MAROON);

    DrawText("Process Count (1-10):", 40, 98, 16, WHITE);
    if (DrawButton((Rectangle){ 230, 93, 30, 25 }, "-", DARKGRAY) && process_count > 1) process_count--;
    DrawText(TextFormat("%d", process_count), 275, 98, 16, YELLOW);
    if (DrawButton((Rectangle){ 300, 93, 30, 25 }, "+", DARKGRAY) && process_count < MAX_PROCESSES) process_count++;

    DrawText("RR Quantum (sec):", 380, 98, 16, WHITE);
    DrawNumericInput((Rectangle){ 530, 93, 60, 25 }, &time_slice, 999);
    if (time_slice < 0.1f) time_slice = 0.1f; // Sanity check bound

    DrawText("Context Switch Overhead (sec, RR/EDF):", 40, 133, 16, WHITE);
    DrawNumericInput((Rectangle){ 380, 128, 60, 25 }, &context_switch_time, 998);
    if (context_switch_time < 0.0f) context_switch_time = 0.0f; // Sanity check bound

    // Save / Launch action buttons
    if (DrawButton((Rectangle){ 460, 165, 150, 36 }, "SAVE CONFIG", DARKBLUE)) {
        active_input_field = -1;
        SaveConfig();
        strcpy(status_message, "Configuration saved!");
        status_message_timer = 1.5f;
    }
    if (DrawButton((Rectangle){ 620, 165, 140, 36 }, "LAUNCH SIMULATION", GREEN)) {
        active_input_field = -1; // clear active selections
        StartSimulation();
    }

    if (status_message_timer > 0) {
        DrawText(status_message, 40, 174, 16, LIME);
    }

    // Table Headers for the batch tasks
    int startY = 215;
    DrawRectangle(40, startY, 720, 30, DARKGRAY);
    DrawText("PID", 55, startY + 7, 14, WHITE);
    DrawText("Arrival Time (s)", 130, startY + 7, 14, WHITE);
    DrawText("Burst Time (s)", 300, startY + 7, 14, WHITE);
    DrawText("Deadline Target (s)", 470, startY + 7, 14, WHITE);
    DrawText("Color Hint", 660, startY + 7, 14, WHITE);

    // Iterating dynamic process table rows
    for (int i = 0; i < process_count; i++) {
        int rowY = startY + 35 + (i * 32);

        // Alternating background stripes for crisp alignment
        DrawRectangle(40, rowY - 4, 720, 30, (i % 2 == 0) ? ColorAlpha(GRAY, 0.1f) : BLANK);

        DrawText(TextFormat("P%d", processes[i].id), 55, rowY, 16, WHITE);

        // Custom ID assignments to track fields dynamically inside arrays
        DrawNumericInput((Rectangle){ 130, rowY - 4, 80, 24 }, &processes[i].arrival_time, (i * 3) + 0);
        DrawNumericInput((Rectangle){ 300, rowY - 4, 80, 24 }, &processes[i].burst_time, (i * 3) + 1);
        DrawNumericInput((Rectangle){ 470, rowY - 4, 80, 24 }, &processes[i].deadline, (i * 3) + 2);

        DrawRectangle(660, rowY - 2, 45, 20, processes[i].color);
    }

    int tipY = startY + 35 + process_count * 32 + 12;
    DrawText("Tip: Click values to edit. Press [Enter] to submit AND save automatically.", 40, tipY, 13, GRAY);
    DrawText("Your values are saved to scheduler_config.txt and reloaded next time you open the app.", 40, tipY + 18, 13, GRAY);
}

void DrawProcesses() {
    DrawText("Process Status:", 10, 58, 16, LIGHTGRAY);

    for (int i = 0; i < process_count; i++) {
        int y = 84 + i * 32;

        DrawText(TextFormat("P%d", processes[i].id), 10, y, 18, WHITE);
        DrawRectangle(100, y, 200, 20, DARKGRAY);

        // Prevent division by zero if burst was configured to 0 manually
        float progress = 0.0f;
        if (processes[i].burst_time > 0) {
            progress = 1.0f - (processes[i].remaining_time / processes[i].burst_time);
        }

        DrawRectangle(100, y, (int)(200 * progress), 20, processes[i].color);

        if (processes[i].finished) {
            float turnaround = processes[i].completion_time - processes[i].arrival_time;
            float waiting = turnaround - processes[i].burst_time;
            DrawText(TextFormat("Arr:%.1f  Rem:%.1f  D:%.1f  |  TAT:%.1f  WT:%.1f  (DONE @%.1f)",
                     processes[i].arrival_time, processes[i].remaining_time, processes[i].deadline,
                     turnaround, waiting, processes[i].completion_time),
                     320, y, 13, GREEN);
        } else {
            DrawText(TextFormat("Arr:%.1f  Rem:%.1f  D:%.1f  |  TAT:--  WT:--",
                     processes[i].arrival_time, processes[i].remaining_time, processes[i].deadline),
                     320, y, 13, LIGHTGRAY);
        }
    }
}

void DrawCPU() {
    int y = 400;
    DrawRectangleLines(40, y, 720, 50, GRAY);
    DrawText("CPU Core Allocation:", 55, y + 17, 16, LIGHTGRAY);

    if (in_context_switch) {
        // Pulsing orange indicator while overhead is being paid
        float pulse = sinf(current_time * 10.0f) * 0.3f + 0.7f;
        DrawRectangle(250, y + 10, 220, 30, ColorAlpha((Color){ 235, 150, 40, 255 }, pulse));
        DrawText(TextFormat("CONTEXT SWITCH... (%.2f/%.2fs) -> P%d",
                 context_timer, context_switch_time, next_process),
                 260, y + 17, 13, BLACK);
    }
    else if (current_process != -1) {
        DrawRectangle(250, y + 10, 120, 30, processes[current_process].color);
        DrawText(TextFormat("PROCESS P%d", current_process), 263, y + 17, 14, BLACK);
    }
    else {
        DrawText("IDLE (No active thread)", 250, y + 17, 16, DARKGRAY);
    }
}

// Scrolling Gantt chart: as current_time advances the visible window scrolls,
// giving continuous left-to-right motion. The currently active slice also pulses.
void DrawGanttChart(float windowSeconds) {
    Rectangle area = { 40, 470, 720, 100 };

    DrawText("Gantt Chart (live, scrolling) - orange = context switch overhead:", area.x, area.y - 20, 14, LIGHTGRAY);
    DrawRectangleRec(area, (Color){ 15, 15, 18, 255 });
    DrawRectangleLinesEx(area, 1, GRAY);

    float windowStart = current_time - windowSeconds;
    if (windowStart < 0) windowStart = 0;
    float windowEnd = windowStart + windowSeconds;

    for (int i = 0; i < ganttCount; i++) {
        GanttEntry* e = &ganttChart[i];
        if (e->end < windowStart) continue; // already scrolled off-screen

        float segStart = (e->start < windowStart) ? windowStart : e->start;
        float segEnd = (e->end > windowEnd) ? windowEnd : e->end;
        if (segEnd <= segStart) continue;

        float x1 = area.x + (segStart - windowStart) / windowSeconds * area.width;
        float x2 = area.x + (segEnd - windowStart) / windowSeconds * area.width;

        Color c = e->color;
        bool isActiveNow = (i == ganttCount - 1) &&
                            ((e->pid == current_process && current_process != -1) ||
                             (e->pid == -2 && in_context_switch));
        if (isActiveNow) {
            float pulse = sinf(current_time * 6.0f) * 0.25f + 0.75f; // breathing highlight
            c = ColorAlpha(c, pulse);
        }

        DrawRectangle((int)x1, (int)area.y + 5, (int)(x2 - x1), (int)area.height - 30, c);
        DrawRectangleLines((int)x1, (int)area.y + 5, (int)(x2 - x1), (int)area.height - 30, BLACK);

        if (e->pid >= 0 && (x2 - x1) > 18) {
            DrawText(TextFormat("P%d", e->pid), x1 + 3, area.y + 8, 12, WHITE);
        } else if (e->pid == -2 && (x2 - x1) > 14) {
            DrawText("CS", x1 + 3, area.y + 8, 12, BLACK);
        }
    }

    // Time axis ticks along the bottom of the chart
    int ticks = (int)windowSeconds;
    for (int t = 0; t <= ticks; t++) {
        float tx = area.x + ((float)t / windowSeconds) * area.width;
        float timeLabel = windowStart + t;
        DrawLine((int)tx, (int)(area.y + area.height - 20), (int)tx, (int)(area.y + area.height - 15), GRAY);
        DrawText(TextFormat("%.0f", timeLabel), (int)tx - 5, (int)(area.y + area.height - 13), 10, GRAY);
    }

    // Pulsing "now" marker at the right edge to reinforce the live motion
    float pulse2 = sinf(current_time * 8.0f) * 0.5f + 0.5f;
    DrawLine((int)(area.x + area.width - 2), (int)area.y, (int)(area.x + area.width - 2), (int)(area.y + area.height),
             ColorAlpha(RED, 0.5f + pulse2 * 0.5f));
}

// Aggregate turnaround/waiting statistics across all finished processes
void DrawAverageStats() {
    float sumTAT = 0, sumWT = 0;
    int count = 0;

    for (int i = 0; i < process_count; i++) {
        if (processes[i].finished) {
            float tat = processes[i].completion_time - processes[i].arrival_time;
            float wt = tat - processes[i].burst_time;
            sumTAT += tat;
            sumWT += wt;
            count++;
        }
    }

    if (count > 0) {
        DrawText(TextFormat("Avg Turnaround: %.2fs   |   Avg Waiting: %.2fs   |   Finished: %d/%d",
                 sumTAT / count, sumWT / count, count, process_count),
                 40, 580, 16, GREEN);
    } else {
        DrawText("Avg Turnaround: --   |   Avg Waiting: --   |   Finished: 0 (no process completed yet)",
                 40, 580, 16, GRAY);
    }
}

int main() {
    InitWindow(800, 680, "Process Scheduler Simulator Architecture");
    SetTargetFPS(60);

    if (!LoadConfig()) {
        InitProcessesDefaults();
    }

    while (!WindowShouldClose()) {
        if (status_message_timer > 0) status_message_timer -= GetFrameTime();

        if (current_state == STATE_MENU) {
            // Logic handled purely within active text element component updates
        }
        else if (current_state == STATE_RUNNING) {
            if (IsKeyPressed(KEY_ONE)) scheduler = FIFO;
            if (IsKeyPressed(KEY_TWO)) scheduler = RR;
            if (IsKeyPressed(KEY_THREE)) scheduler = EDF;
            if (IsKeyPressed(KEY_FOUR)) scheduler = CTS;

            // Allow backtracking to configuration panel seamlessly
            if (IsKeyPressed(KEY_ESCAPE)) current_state = STATE_MENU;

            UpdateScheduler(GetFrameTime());
        }

        BeginDrawing();
        ClearBackground((Color){ 20, 20, 25, 255 });

        if (current_state == STATE_MENU) {
            DrawMenuPage();
        }
        else if (current_state == STATE_RUNNING) {
            DrawText("1:FIFO  2:RR  3:EDF  4:CTS  [Esc]:Config Menu", 10, 15, 16, LIGHTGRAY);

            const char* schedNames[] = { "First-In First-Out", "Round Robin", "Earliest Deadline First", "Completely Fair (CTS)" };
            DrawText(TextFormat("Active Core Engine: %s", schedNames[scheduler]), 10, 36, 14, YELLOW);

            if (scheduler == RR) {
                DrawText(TextFormat("Quantum: %.1fs | Slice Timer: %.1fs | CS overhead: %.2fs", time_slice, rr_timer, context_switch_time), 400, 36, 13, RAYWHITE);
            } else if (scheduler == EDF) {
                DrawText(TextFormat("CS overhead: %.2fs (on every arrival)", context_switch_time), 400, 36, 13, RAYWHITE);
            }

            DrawText(TextFormat("Global Clock: %.2fs", current_time), 630, 15, 16, GREEN);

            DrawProcesses();
            DrawCPU();
            DrawGanttChart(15.0f); // shows a rolling 15-second window
            DrawAverageStats();

            if (AllProcessesFinished()) {
                DrawText("Simulation Finished", 280, 320, 24, GREEN);
            }
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}