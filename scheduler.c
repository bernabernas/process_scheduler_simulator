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
    CTS,
    MOB   // Multi-Objective scoring scheduler
} SchedulerType;

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
    float priority;       // user-defined priority (1-10, higher = more important)
    float score;          // computed each frame when MOB is active
    Color color;

    // --- Stats tracking ---
    bool finished;
    float completion_time;
} Process;

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
float time_slice = 1.0f;
Process processes[MAX_PROCESSES];

float current_time = 0;
int current_process = -1;
static int rrIndex = -1;
SchedulerType scheduler = FIFO;
float rr_timer = 0;
float edf_timer = 0;
AppState current_state = STATE_MENU;

// Context switch overhead (RR, EDF, MOB)
float context_switch_time = 0.3f;
float context_timer = 0.0f;
bool in_context_switch = false;
int next_process = -1;

// MOB weights  (w1..w5 in the formula)
float w1 = 0.30f;  // Waiting Time
float w2 = 0.35f;  // Deadline Urgency  (1 / time_to_deadline)
float w3 = 0.20f;  // Burst urgency     (1 / remaining_time)
float w4 = 0.15f;  // Priority
float w5 = 0.10f;  // Context-switch penalty (subtracted if switching away from current)

// Input system
int active_input_field = -1;
char input_buffer[16] = { 0 };

// Gantt history
GanttEntry ganttChart[MAX_GANTT_ENTRIES];
int ganttCount = 0;

// HUD feedback
char status_message[64] = { 0 };
float status_message_timer = 0;
bool algorithm_chosen = false;  // user must press 1-5 before simulation ticks

// ============================================================
//  Process defaults
// ============================================================
void InitProcessesDefaults() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].id            = i;
        processes[i].arrival_time  = i * 1.5f;
        processes[i].burst_time    = 3.0f + (i % 3) * 2.0f;
        processes[i].remaining_time= processes[i].burst_time;
        processes[i].deadline      = processes[i].arrival_time + 10.0f;
        processes[i].vruntime      = 0;
        processes[i].priority      = (float)(i % 5 + 1);  // 1..5 default
        processes[i].score         = 0;
        processes[i].finished      = false;
        processes[i].completion_time = 0;
        processes[i].color = (Color){
            (unsigned char)(50  + (i * 40) % 205),
            (unsigned char)(80  + (i * 60) % 175),
            (unsigned char)(100 + (i * 30) % 155),
            255
        };
    }
}

static void FillDefaultProcess(int i) {
    processes[i].id            = i;
    processes[i].arrival_time  = i * 1.5f;
    processes[i].burst_time    = 3.0f + (i % 3) * 2.0f;
    processes[i].remaining_time= processes[i].burst_time;
    processes[i].deadline      = processes[i].arrival_time + 10.0f;
    processes[i].vruntime      = 0;
    processes[i].priority      = (float)(i % 5 + 1);
    processes[i].score         = 0;
    processes[i].finished      = false;
    processes[i].completion_time = 0;
    processes[i].color = (Color){
        (unsigned char)(50  + (i * 40) % 205),
        (unsigned char)(80  + (i * 60) % 175),
        (unsigned char)(100 + (i * 30) % 155),
        255
    };
}

// ============================================================
//  Persistence
// ============================================================
bool SaveConfig() {
    FILE* f = fopen(CONFIG_FILENAME, "w");
    if (!f) return false;

    // header line: counts + global floats
    fprintf(f, "%d %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
            process_count, time_slice, context_switch_time,
            w1, w2, w3, w4, w5);

    for (int i = 0; i < process_count; i++) {
        fprintf(f, "%.3f %.3f %.3f %.3f\n",
                processes[i].arrival_time,
                processes[i].burst_time,
                processes[i].deadline,
                processes[i].priority);
    }
    fclose(f);
    return true;
}

bool LoadConfig() {
    FILE* f = fopen(CONFIG_FILENAME, "r");
    if (!f) return false;

    int pc;
    float ts, cst, lw1, lw2, lw3, lw4, lw5;
    // SaveConfig writes exactly 8 fields on the header line.
    // Read exactly 8 — no more, no less — so the file pointer lands
    // at the start of the per-process rows, not one field ahead.
    if (fscanf(f, "%d %f %f %f %f %f %f %f",
               &pc, &ts, &cst, &lw1, &lw2, &lw3, &lw4, &lw5) != 8
        || pc < 1 || pc > MAX_PROCESSES) {
        fclose(f);
        return false;
    }

    process_count       = pc;
    time_slice          = ts;
    context_switch_time = fmaxf(0.0f, cst);
    w1 = lw1; w2 = lw2; w3 = lw3; w4 = lw4; w5 = lw5;

    for (int i = 0; i < process_count; i++) {
        float a, b, d, p;
        if (fscanf(f, "%f %f %f %f", &a, &b, &d, &p) != 4) {
            fclose(f);
            InitProcessesDefaults();
            return false;
        }
        processes[i].id             = i;
        processes[i].arrival_time   = a;
        processes[i].burst_time     = b;
        processes[i].remaining_time = b;
        processes[i].deadline       = d;
        processes[i].priority       = p;
        processes[i].vruntime       = 0;
        processes[i].score          = 0;
        processes[i].finished       = false;
        processes[i].completion_time= 0;
        processes[i].color = (Color){
            (unsigned char)(50  + (i * 40) % 205),
            (unsigned char)(80  + (i * 60) % 175),
            (unsigned char)(100 + (i * 30) % 155),
            255
        };
    }

    for (int i = process_count; i < MAX_PROCESSES; i++) FillDefaultProcess(i);

    fclose(f);
    return true;
}

// ============================================================
//  Simulation reset
// ============================================================
void StartSimulation() {
    current_time   = 0;
    current_process= -1;
    rr_timer = 0;
    edf_timer = 0;
    ganttCount = 0;
    rrIndex = -1;
    in_context_switch = false;
    context_timer = 0;
    next_process = -1;

    for (int i = 0; i < process_count; i++) {
        processes[i].remaining_time  = processes[i].burst_time;
        processes[i].vruntime        = 0;
        processes[i].score           = 0;
        processes[i].finished        = false;
        processes[i].completion_time = 0;
    }

    algorithm_chosen = false;
    SaveConfig();
    current_state = STATE_RUNNING;
}

bool AllProcessesFinished() {
    for (int i = 0; i < process_count; i++)
        if (!processes[i].finished) return false;
    return true;
}

// ============================================================
//  Selectors
// ============================================================
int SelectFIFO() {
    for (int i = 0; i < process_count; i++)
        if (processes[i].arrival_time <= current_time && processes[i].remaining_time > 0)
            return i;
    return -1;
}

int SelectRR() {
    for (int i = 0; i < process_count; i++) {
        rrIndex = (rrIndex + 1) % process_count;
        if (processes[rrIndex].arrival_time <= current_time &&
            processes[rrIndex].remaining_time > 0)
            return rrIndex;
    }
    return -1;
}

int SelectEDF() {
    int best = -1;
    float min_dl = 1e9f;
    for (int i = 0; i < process_count; i++) {
        if (processes[i].arrival_time <= current_time &&
            processes[i].remaining_time > 0 &&
            processes[i].deadline < min_dl) {
            min_dl = processes[i].deadline;
            best = i;
        }
    }
    return best;
}

int SelectCTS() {
    int best = -1;
    float min_vr = 1e9f;
    for (int i = 0; i < process_count; i++) {
        if (processes[i].arrival_time <= current_time &&
            processes[i].remaining_time > 0 &&
            processes[i].vruntime < min_vr) {
            min_vr = processes[i].vruntime;
            best = i;
        }
    }
    return best;
}

// Multi-Objective scoring:
//   Score = w1*WaitingTime
//         + w2*DeadlineUrgency   (= 1 / max(deadline - now, 0.01))
//         + w3*(1/BurstRemaining)
//         + w4*Priority
//         - w5*ContextSwitchPenalty  (1 if candidate != current_process, else 0)
//
// All components are computed relative to the ready-process set, then the
// candidate with the highest score wins.
int SelectMOB() {
    // First pass: compute raw component values for every ready process
    float maxWait = 0, maxUrgency = 0, maxBurstInv = 0, maxPrio = 0;

    for (int i = 0; i < process_count; i++) {
        if (processes[i].arrival_time > current_time || processes[i].remaining_time <= 0)
            continue;

        float wait      = current_time - processes[i].arrival_time;
        float urgency   = 1.0f / fmaxf(processes[i].deadline - current_time, 0.01f);
        float burstInv  = 1.0f / fmaxf(processes[i].remaining_time, 0.01f);

        if (wait     > maxWait)     maxWait     = wait;
        if (urgency  > maxUrgency)  maxUrgency  = urgency;
        if (burstInv > maxBurstInv) maxBurstInv = burstInv;
        if (processes[i].priority > maxPrio) maxPrio = processes[i].priority;
    }

    // Avoid division by zero during normalisation
    if (maxWait     < 1e-6f) maxWait     = 1.0f;
    if (maxUrgency  < 1e-6f) maxUrgency  = 1.0f;
    if (maxBurstInv < 1e-6f) maxBurstInv = 1.0f;
    if (maxPrio     < 1e-6f) maxPrio     = 1.0f;

    int best = -1;
    float bestScore = -1e9f;

    for (int i = 0; i < process_count; i++) {
        if (processes[i].arrival_time > current_time || processes[i].remaining_time <= 0)
            continue;

        float waitNorm    = (current_time - processes[i].arrival_time) / maxWait;
        float urgencyNorm = (1.0f / fmaxf(processes[i].deadline - current_time, 0.01f)) / maxUrgency;
        float burstNorm   = (1.0f / fmaxf(processes[i].remaining_time, 0.01f)) / maxBurstInv;
        float prioNorm    = processes[i].priority / maxPrio;
        float csPenalty   = (i != current_process) ? 1.0f : 0.0f;

        float s = w1 * waitNorm
                + w2 * urgencyNorm
                + w3 * burstNorm
                + w4 * prioNorm
                - w5 * csPenalty;

        processes[i].score = s;

        if (s > bestScore) {
            bestScore = s;
            best = i;
        }
    }

    return best;
}

// ============================================================
//  Gantt helpers
// ============================================================
static void LogGanttSlice(float t0, float t1, int pid) {
    if (ganttCount > 0 && ganttChart[ganttCount - 1].pid == pid) {
        ganttChart[ganttCount - 1].end = t1;
        return;
    }
    if (ganttCount >= MAX_GANTT_ENTRIES) return;

    Color c;
    if      (pid == -1) c = (Color){ 60, 60,  60, 255 };   // idle
    else if (pid == -2) c = (Color){ 235,150, 40, 255 };   // context switch
    else                c = processes[pid].color;

    ganttChart[ganttCount] = (GanttEntry){ pid, t0, t1, c };
    ganttCount++;
}

static void StartContextSwitch(int target) {
    in_context_switch = true;
    next_process      = target;
    context_timer     = 0;
    current_process   = -1;
}

// ============================================================
//  Scheduler update  (called every frame)
// ============================================================
void UpdateScheduler(float dt) {

    // --- Overhead period: CPU blocked, no useful work ---
    if (in_context_switch) {
        context_timer += dt;
        current_time  += dt;
        LogGanttSlice(current_time - dt, current_time, -2);

        if (context_timer >= context_switch_time) {
            current_process = next_process;
            next_process    = -1;
            context_timer   = 0;
            in_context_switch = false;

            if (scheduler == RR)  rr_timer  = 0;
            if (scheduler == EDF) edf_timer = 0;
        }
        return;
    }

    if (AllProcessesFinished()) { current_process = -1; return; }

    float old_time       = current_time;
    current_time        += dt;
    int previous_process = current_process;

    // Helper lambda-style: trigger a switch only when a real process was running
    // (not after a natural completion or a fresh start from idle)
    bool switching    = false;
    int  switchTarget = -1;

    // ---- FIFO ----
    if (scheduler == FIFO) {
        current_process = SelectFIFO();
    }
    // ---- Round Robin ----
    else if (scheduler == RR) {
        rr_timer += dt;

        if (current_process == -1) {
            current_process = SelectRR();
            rr_timer = 0;
        } else if (rr_timer >= time_slice) {
            int candidate = SelectRR();
            rr_timer = 0;
            if (candidate != -1) { switching = true; switchTarget = candidate; }
        }
    }
    // ---- EDF com quantum (EDF + RR) ----
    // Dois gatilhos independentes disparam CS overhead:
    //   1. Quantum expirou  → re-seleciona por deadline e paga overhead (como RR).
    //   2. Chegou processo com deadline mais urgente → preempta com overhead.
    // Conclusao natural do processo nunca paga overhead (current_process == -1).
    else if (scheduler == EDF) {
        edf_timer += dt;

        int candidate = SelectEDF();

        if (current_process == -1) {
            // CPU ociosa: seleciona direto, sem overhead.
            current_process = candidate;
            edf_timer = 0;
        } else if (candidate != -1 && candidate != current_process) {
            // Preempcao por deadline: processo mais urgente chegou.
            switching    = true;
            switchTarget = candidate;
            edf_timer    = 0;
        } else if (edf_timer >= time_slice) {
            // Quantum expirou: CS obrigatorio como no RR, vencedor e o de menor deadline.
            edf_timer = 0;
            if (candidate != -1) { switching = true; switchTarget = candidate; }
        }
    }
    // ---- CTS (Completely Fair) ----
    // Preemptivo: reavalia todo frame. Qualquer processo que chegue com vruntime
    // menor que o atual deve preemptar imediatamente — exatamente como o CFS real.
    // Não usa context-switch overhead (troca é "gratuita" como no kernel).
    else if (scheduler == CTS) {
        int candidate = SelectCTS();

        if (current_process == -1) {
            // CPU estava ociosa: seleciona diretamente, sem overhead.
            current_process = candidate;
        } else if (candidate != -1 && candidate != current_process) {
            // Preempção: credita o processo ANTERIOR pelo trabalho deste frame
            // antes de trocar — corrige o bug de atribuição de vruntime.
            processes[previous_process].vruntime       += dt;
            processes[previous_process].remaining_time -= dt;

            if (processes[previous_process].remaining_time <= 0) {
                // Terminou exatamente na preempção: registra conclusão e sai.
                processes[previous_process].remaining_time  = 0;
                processes[previous_process].finished        = true;
                processes[previous_process].completion_time = current_time;
                current_process = -1;
                LogGanttSlice(old_time, current_time, previous_process);
                return;
            }

            LogGanttSlice(old_time, current_time, previous_process);
            current_process = candidate;   // troca instantânea, sem overhead
            return;
        }
        // Mesmo processo continua: cai no bloco de execução normal abaixo.
    }
    // ---- MOB (Multi-Objective) ----
    else if (scheduler == MOB) {
        // Re-score every frame (preemptive); fire a context switch whenever the
        // winner differs from whoever was running.  The w5 penalty term already
        // discourages unnecessary switches inside the score formula itself.
        int candidate = SelectMOB();

        if (current_process == -1) {
            current_process = candidate;   // fresh start, no overhead
        } else if (candidate != -1 && candidate != current_process) {
            switching    = true;
            switchTarget = candidate;
        }
    }

    // ---- Handle a preemptive switch ----
    if (switching) {
        if (previous_process != -1) {
            processes[previous_process].remaining_time -= dt;

            if (processes[previous_process].remaining_time <= 0) {
                // Process finished exactly at switch boundary: no overhead
                processes[previous_process].remaining_time  = 0;
                processes[previous_process].finished        = true;
                processes[previous_process].completion_time = current_time;
                current_process = -1;
                LogGanttSlice(old_time, current_time, previous_process);
                return;
            }
        }
        LogGanttSlice(old_time, current_time, previous_process);
        StartContextSwitch(switchTarget);
        return;
    }

    // ---- Normal execution this frame ----
    LogGanttSlice(old_time, current_time, current_process);

    if (current_process != -1) {
        processes[current_process].remaining_time -= dt;

        if (scheduler == CTS) processes[current_process].vruntime += dt;

        if (processes[current_process].remaining_time <= 0) {
            processes[current_process].remaining_time  = 0;
            processes[current_process].finished        = true;
            processes[current_process].completion_time = current_time;
            current_process = -1;
        }
    }
}

// ============================================================
//  UI helpers
// ============================================================
bool DrawButton(Rectangle b, const char* text, Color baseColor) {
    Vector2 mp = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mp, b);
    bool clicked = hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    DrawRectangleRec(b, hovered ? ColorAlpha(baseColor, 0.8f) : baseColor);
    DrawRectangleLinesEx(b, 1, WHITE);

    int fw = MeasureText(text, 16);
    DrawText(text, b.x + (b.width - fw)/2, b.y + (b.height - 16)/2, 16, WHITE);
    return clicked;
}

void DrawNumericInput(Rectangle rect, float* value, int fieldId) {
    Vector2 mp = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mp, rect);

    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        active_input_field = fieldId;
        snprintf(input_buffer, sizeof(input_buffer), "%.2f", *value);
    }

    DrawRectangleRec(rect, (active_input_field == fieldId) ? MAROON
                         : (hovered             ? DARKGRAY : BLACK));
    DrawRectangleLinesEx(rect, 1, GRAY);

    if (active_input_field == fieldId) {
        int key = GetCharPressed();
        while (key > 0) {
            if ((key >= '0' && key <= '9') || key == '.') {
                int len = strlen(input_buffer);
                if (len < (int)sizeof(input_buffer) - 1) {
                    input_buffer[len]   = (char)key;
                    input_buffer[len+1] = '\0';
                }
            }
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int len = strlen(input_buffer);
            if (len > 0) input_buffer[len-1] = '\0';
        }
        if (IsKeyPressed(KEY_ENTER)) {
            *value = strtof(input_buffer, NULL);
            active_input_field = -1;
            SaveConfig();
            strcpy(status_message, "Configuration saved!");
            status_message_timer = 1.5f;
        }
        DrawText(TextFormat("%s|", input_buffer), rect.x+4, rect.y+4, 14, YELLOW);
    } else {
        DrawText(TextFormat("%.2f", *value), rect.x+4, rect.y+4, 14, WHITE);
    }
}

// ============================================================
//  Menu page
// ============================================================
void DrawMenuPage() {
    DrawText("Process Scheduler Configurator", 40, 18, 26, LIGHTGRAY);
    DrawText("System Settings:", 40, 60, 18, MAROON);

    // Row 1 – process count + RR quantum
    DrawText("Processes:", 40, 93, 15, WHITE);
    if (DrawButton((Rectangle){145,89,26,24},"-",DARKGRAY) && process_count > 1) process_count--;
    DrawText(TextFormat("%d", process_count), 179, 93, 15, YELLOW);
    if (DrawButton((Rectangle){200,89,26,24},"+",DARKGRAY) && process_count < MAX_PROCESSES) process_count++;

    DrawText("RR Quantum:", 250, 93, 15, WHITE);
    DrawNumericInput((Rectangle){360,89,58,24}, &time_slice, 9901);
    if (time_slice < 0.1f) time_slice = 0.1f;

    DrawText("CS Overhead:", 440, 93, 15, WHITE);
    DrawNumericInput((Rectangle){555,89,58,24}, &context_switch_time, 9902);
    if (context_switch_time < 0.0f) context_switch_time = 0.0f;

    // Row 2 – MOB weights label
    DrawText("MOB Weights  (w1=Wait  w2=Deadline  w3=Burst  w4=Priority  w5=Switch Penalty):",
             40, 124, 13, (Color){200,200,80,255});

    // Row 3 – five weight inputs
    const char* wLabels[] = {"w1","w2","w3","w4","w5"};
    float* wPtrs[]        = {&w1, &w2, &w3, &w4, &w5};
    for (int k = 0; k < 5; k++) {
        int x = 40 + k * 146;
        DrawText(wLabels[k], x, 147, 14, LIGHTGRAY);
        DrawNumericInput((Rectangle){x+24, 143, 58, 24}, wPtrs[k], 9910+k);
        if (*wPtrs[k] < 0.0f) *wPtrs[k] = 0.0f;
        if (*wPtrs[k] > 9.9f) *wPtrs[k] = 9.9f;
    }

    // Action buttons
    if (DrawButton((Rectangle){470,173,140,32},"SAVE CONFIG",DARKBLUE)) {
        active_input_field = -1;
        SaveConfig();
        strcpy(status_message, "Configuration saved!");
        status_message_timer = 1.5f;
    }
    if (DrawButton((Rectangle){622,173,138,32},"LAUNCH SIMULATION",GREEN)) {
        active_input_field = -1;
        StartSimulation();
    }
    if (status_message_timer > 0)
        DrawText(status_message, 40, 178, 14, LIME);

    // Process table
    int startY = 217;
    DrawRectangle(40, startY, 720, 28, DARKGRAY);
    DrawText("PID",   54, startY+7, 13, WHITE);
    DrawText("Arrival",  110, startY+7, 13, WHITE);
    DrawText("Burst",    220, startY+7, 13, WHITE);
    DrawText("Deadline", 330, startY+7, 13, WHITE);
    DrawText("Priority", 450, startY+7, 13, WHITE);
    DrawText("Color",    570, startY+7, 13, WHITE);

    for (int i = 0; i < process_count; i++) {
        int rowY = startY + 30 + i * 30;
        DrawRectangle(40, rowY-2, 720, 28,
            (i % 2 == 0) ? ColorAlpha(GRAY, 0.08f) : BLANK);

        DrawText(TextFormat("P%d", i), 54, rowY+3, 14, WHITE);

        DrawNumericInput((Rectangle){110, rowY, 80, 23}, &processes[i].arrival_time, (i*4)+0);
        DrawNumericInput((Rectangle){220, rowY, 80, 23}, &processes[i].burst_time,   (i*4)+1);
        DrawNumericInput((Rectangle){330, rowY, 80, 23}, &processes[i].deadline,     (i*4)+2);
        DrawNumericInput((Rectangle){450, rowY, 60, 23}, &processes[i].priority,     (i*4)+3);
        if (processes[i].priority < 0.1f) processes[i].priority = 0.1f;
        if (processes[i].priority > 10.f) processes[i].priority = 10.f;

        DrawRectangle(570, rowY+2, 40, 18, processes[i].color);
    }

    int tipY = startY + 30 + process_count * 30 + 10;
    DrawText("Tip: [Enter] to commit a field and auto-save. Values reload on next launch.", 40, tipY, 12, GRAY);
    DrawText("MOB scores are normalised [0-1] per component then weighted.", 40, tipY+16, 12, (Color){160,160,60,255});
}

// ============================================================
//  Simulation drawing
// ============================================================
void DrawProcesses() {
    DrawText("Process Status:", 10, 56, 15, LIGHTGRAY);

    bool showScore = (scheduler == MOB);

    for (int i = 0; i < process_count; i++) {
        int y = 78 + i * 30;

        // PID label
        Color pidColor = (i == current_process && !in_context_switch) ?
                         YELLOW : WHITE;
        DrawText(TextFormat("P%d", i), 10, y, 16, pidColor);

        // Progress bar
        DrawRectangle(48, y+1, 160, 18, DARKGRAY);
        float progress = 0;
        if (processes[i].burst_time > 0)
            progress = 1.0f - processes[i].remaining_time / processes[i].burst_time;
        DrawRectangle(48, y+1, (int)(160 * progress), 18, processes[i].color);

        // Stats text
        if (processes[i].finished) {
            float tat = processes[i].completion_time - processes[i].arrival_time;
            float wt  = tat - processes[i].burst_time;
            Color color;
            if(tat > processes[i].deadline - processes[i].arrival_time) color = RED;
            else color = GREEN;
            DrawText(TextFormat("Arr:%.1f Rem:%.1f D:%.1f | TAT:%.1f WT:%.1f  DONE@%.1fs",
                     processes[i].arrival_time, processes[i].remaining_time,
                     processes[i].deadline, tat, wt, processes[i].completion_time),
                     218, y+2, 12, color);
        } else if (showScore) {
            DrawText(TextFormat("Arr:%.1f Rem:%.1f D:%.1f Pri:%.0f | Score:%.3f",
                     processes[i].arrival_time, processes[i].remaining_time,
                     processes[i].deadline, processes[i].priority, processes[i].score),
                     218, y+2, 12, LIGHTGRAY);
        } else {
            DrawText(TextFormat("Arr:%.1f Rem:%.1f D:%.1f Pri:%.0f",
                     processes[i].arrival_time, processes[i].remaining_time,
                     processes[i].deadline, processes[i].priority),
                     218, y+2, 12, LIGHTGRAY);
        }
    }
}

void DrawCPU() {
    int y = 390;
    DrawRectangleLines(40, y, 720, 48, GRAY);
    DrawText("CPU Core:", 55, y+15, 15, LIGHTGRAY);

    if (in_context_switch) {
        float pulse = sinf(current_time * 10.0f) * 0.3f + 0.7f;
        DrawRectangle(175, y+9, 280, 30, ColorAlpha((Color){235,150,40,255}, pulse));
        DrawText(TextFormat("CONTEXT SWITCH (%.2f/%.2fs)  next: P%d",
                 context_timer, context_switch_time, next_process),
                 183, y+17, 13, BLACK);
    } else if (current_process != -1) {
        DrawRectangle(175, y+9, 120, 30, processes[current_process].color);
        DrawText(TextFormat("P%d  running", current_process), 185, y+17, 14, BLACK);
    } else {
        DrawText("IDLE", 175, y+15, 15, DARKGRAY);
    }
}

void DrawGanttChart(float windowSec) {
    Rectangle area = {40, 452, 720, 96};

    DrawText("Gantt  (orange = context switch overhead):", area.x, area.y-18, 13, LIGHTGRAY);
    DrawRectangleRec(area, (Color){14,14,17,255});
    DrawRectangleLinesEx(area, 1, GRAY);

    float wStart = fmaxf(0, current_time - windowSec);
    float wEnd   = wStart + windowSec;

    for (int i = 0; i < ganttCount; i++) {
        GanttEntry* e = &ganttChart[i];
        if (e->end < wStart) continue;

        float s  = fmaxf(e->start, wStart);
        float en = fminf(e->end,   wEnd);
        if (en <= s) continue;

        float x1 = area.x + (s  - wStart) / windowSec * area.width;
        float x2 = area.x + (en - wStart) / windowSec * area.width;

        Color c = e->color;
        bool active = (i == ganttCount-1) &&
                      ((e->pid == current_process && current_process != -1) ||
                       (e->pid == -2 && in_context_switch));
        if (active) c = ColorAlpha(c, sinf(current_time * 6.0f)*0.25f + 0.75f);

        DrawRectangle((int)x1, (int)area.y+4, (int)(x2-x1), (int)area.height-26, c);
        DrawRectangleLines((int)x1, (int)area.y+4, (int)(x2-x1), (int)area.height-26, BLACK);

        if (e->pid >= 0 && (x2-x1) > 16)
            DrawText(TextFormat("P%d", e->pid), x1+3, area.y+6, 11, WHITE);
        else if (e->pid == -2 && (x2-x1) > 12)
            DrawText("CS", x1+2, area.y+6, 11, BLACK);
    }

    // Time axis
    for (int t = 0; t <= (int)windowSec; t++) {
        float tx = area.x + ((float)t/windowSec)*area.width;
        DrawLine((int)tx,(int)(area.y+area.height-18),(int)tx,(int)(area.y+area.height-13),GRAY);
        DrawText(TextFormat("%.0f", wStart+t),(int)tx-4,(int)(area.y+area.height-12),9,GRAY);
    }

    // Live edge marker
    float p2 = sinf(current_time*8.0f)*0.5f+0.5f;
    DrawLine((int)(area.x+area.width-2),(int)area.y,
             (int)(area.x+area.width-2),(int)(area.y+area.height),
             ColorAlpha(RED, 0.5f+p2*0.5f));
}

void DrawAverageStats() {
    float sumTAT = 0, sumWT = 0;
    int   count  = 0;

    for (int i = 0; i < process_count; i++) {
        if (processes[i].finished) {
            float tat = processes[i].completion_time - processes[i].arrival_time;
            sumTAT += tat;
            sumWT  += tat - processes[i].burst_time;
            count++;
        }
    }

    if (count > 0)
        DrawText(TextFormat("Avg Turnaround: %.2fs  |  Avg Waiting: %.2fs  |  Finished: %d/%d",
                 sumTAT/count, sumWT/count, count, process_count),
                 40, 558, 15, GREEN);
    else
        DrawText("Avg Turnaround: --   |   Avg Waiting: --   |   No process finished yet",
                 40, 558, 15, GRAY);
}

// Draw MOB weight legend when that scheduler is active
void DrawMOBLegend() {
    int x = 40, y = 578;
    DrawText(TextFormat(
        "MOB Score = %.2f*Wait + %.2f*Deadline + %.2f*Burst + %.2f*Priority - %.2f*SwitchPenalty",
        w1, w2, w3, w4, w5),
        x, y, 13, (Color){200,200,80,255});
}

// ============================================================
//  main
// ============================================================
int main() {
    InitWindow(800, 640, "Process Scheduler Simulator");
    SetTargetFPS(60);

    if (!LoadConfig()) InitProcessesDefaults();

    while (!WindowShouldClose()) {
        if (status_message_timer > 0) status_message_timer -= GetFrameTime();

        if (current_state == STATE_RUNNING) {
            if (IsKeyPressed(KEY_ONE))   { scheduler = FIFO; algorithm_chosen = true; }
            if (IsKeyPressed(KEY_TWO))   { scheduler = RR;   algorithm_chosen = true; }
            if (IsKeyPressed(KEY_THREE)) { scheduler = EDF;  algorithm_chosen = true; }
            if (IsKeyPressed(KEY_FOUR))  { scheduler = CTS;  algorithm_chosen = true; }
            if (IsKeyPressed(KEY_FIVE))  { scheduler = MOB;  algorithm_chosen = true; }
            if (IsKeyPressed(KEY_ESCAPE)) current_state = STATE_MENU;

            if (algorithm_chosen) UpdateScheduler(GetFrameTime());
        }

        BeginDrawing();
        ClearBackground((Color){20,20,25,255});

        if (current_state == STATE_MENU) {
            DrawMenuPage();
        } else {
            // Top bar
            DrawText("1:FIFO  2:RR  3:EDF  4:CTS  5:MOB  [Esc]:Config", 10, 13, 15, LIGHTGRAY);

            const char* names[] = {
                "First-In First-Out","Round Robin",
                "Earliest Deadline First","Completely Fair (CTS)",
                "Multi-Objective (MOB)"
            };
            DrawText(TextFormat("Scheduler: %s", names[scheduler]), 10, 33, 14, YELLOW);

            if (scheduler == RR)
                DrawText(TextFormat("Q:%.1fs  Slice:%.1fs  CS:%.2fs",
                         time_slice, rr_timer, context_switch_time), 430, 33, 13, RAYWHITE);
            else if (scheduler == EDF)
                DrawText(TextFormat("Q:%.1fs  Slice:%.1fs  CS:%.2fs",
                         time_slice, edf_timer, context_switch_time), 430, 33, 13, RAYWHITE);
            else if (scheduler == MOB)
                DrawText(TextFormat("CS overhead: %.2fs", context_switch_time), 430, 33, 13, RAYWHITE);

            DrawText(TextFormat("Clock: %.2fs", current_time), 680, 13, 15, GREEN);

            DrawProcesses();
            DrawCPU();
            DrawGanttChart(15.0f);
            DrawAverageStats();

            if (scheduler == MOB) DrawMOBLegend();

            if (!algorithm_chosen) {
                // Pulsing prompt overlay
                float pulse = sinf(GetTime() * 3.0f) * 0.4f + 0.6f;
                DrawRectangle(0, 0, 800, 640, ColorAlpha((Color){10,10,15,255}, 0.82f));
                DrawText("Choose a scheduling algorithm to start:",
                         170, 240, 20, LIGHTGRAY);
                DrawRectangle(190, 278, 420, 170, (Color){30,30,38,255});
                DrawRectangleLinesEx((Rectangle){190,278,420,170}, 1,
                                     ColorAlpha(GRAY, pulse));
                const char* opts[] = {
                    "  1   FIFO   —  First-In First-Out",
                    "  2   RR    —  Round Robin",
                    "  3   EDF   —  Earliest Deadline First",
                    "  4   CTS   —  Completely Fair",
                    "  5   MOB   —  Multi-Objective"
                };
                for (int k = 0; k < 5; k++)
                    DrawText(opts[k], 210, 290 + k * 30, 17,
                             ColorAlpha(YELLOW, pulse));
            }

            if (AllProcessesFinished())
                DrawText("** Simulation Finished **",
                    270, (int)(GetScreenHeight()/2) - 14, 24, GREEN);
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}