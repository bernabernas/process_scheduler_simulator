#include "raylib.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_PROCESSES 10

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
} Process;

// --- Configurable Variables (Modified via Home Page) ---
int process_count = 5;
float time_slice = 1.0f; // Global quantum variable
Process processes[MAX_PROCESSES];

float current_time = 0;
int current_process = -1;
SchedulerType scheduler = FIFO;
float rr_timer = 0;
AppState current_state = STATE_MENU;

// Tracks which property string a user is currently typing into
int active_input_field = -1; 
char input_buffer[16] = { 0 };

void InitProcessesDefaults() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].id = i;
        processes[i].arrival_time = i * 1.5f;
        processes[i].burst_time = 3.0f + (i % 3) * 2.0f;
        processes[i].remaining_time = processes[i].burst_time;
        processes[i].deadline = processes[i].arrival_time + 10.0f;
        processes[i].vruntime = 0;
        processes[i].color = (Color){
            (unsigned char)(50 + (i * 40) % 205), 
            (unsigned char)(80 + (i * 60) % 175), 
            (unsigned char)(100 + (i * 30) % 155), 
            255
        };
    }
}

// Reset functions for transitioning from Menu -> Simulation safely
void StartSimulation() {
    current_time = 0;
    current_process = -1;
    rr_timer = 0;
    for (int i = 0; i < process_count; i++) {
        processes[i].remaining_time = processes[i].burst_time;
        processes[i].vruntime = 0;
    }
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

int SelectRR() {
    static int last = -1;
    
    // Reset condition if current process finishes early or becomes invalid
    if (current_process == -1) {
        for (int i = 0; i < process_count; i++) {
            int idx = (last + i + 1) % process_count;
            if (processes[idx].arrival_time <= current_time && processes[idx].remaining_time > 0) {
                last = idx;
                return idx;
            }
        }
        return -1;
    }
    return current_process;
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

void UpdateScheduler(float dt) {
    current_time += dt;

    if (scheduler == RR) {
        rr_timer += dt;
        // If quantum expires OR no process is currently running, find the next slice
        if (rr_timer >= time_slice || current_process == -1 || processes[current_process].remaining_time <= 0) {
            current_process = SelectRR();
            rr_timer = 0;
        }
    } else {
        current_process = SelectProcess();
    }

    if (current_process != -1) {
        processes[current_process].remaining_time -= dt;

        if (scheduler == CTS) {
            processes[current_process].vruntime += dt;
        }

        if (processes[current_process].remaining_time <= 0) {
            processes[current_process].remaining_time = 0;
            current_process = -1; // Force re-evaluation next frame
        }
    }
}

// Helper UI function to render a custom text button component
bool DrawButton(Rectangle bounds, const char* text, Color baseColor) {
    Vector2 mousePos = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mousePos, bounds);
    bool clicked = hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    
    DrawRectangleRec(bounds, hovered ? ColorAlpha(baseColor, 0.8f) : baseColor);
    DrawRectangleLinesEx(bounds, 1, WHITE);
    
    int fontSize = 16;
    int textWidth = MeasureText(text, fontSize);
    DrawText(text, bounds.x + (bounds.width - textWidth)/2, bounds.y + (bounds.height - fontSize)/2, fontSize, WHITE);
    
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
                    input_buffer[len+1] = '\0';
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
        }
        DrawText(TextFormat("%s|", input_buffer), rect.x + 5, rect.y + 5, 16, YELLOW);
    } else {
        DrawText(TextFormat("%.1f", *value), rect.x + 5, rect.y + 5, 16, WHITE);
    }
}

void DrawMenuPage() {
    DrawText("Process Scheduler Configurator", 40, 25, 28, LIGHTGRAY);
    
    // Global Parameter controls
    DrawText("System Settings:", 40, 80, 18, MAROON);
    
    DrawText("Process Count (1-10):", 40, 115, 16, WHITE);
    if (DrawButton((Rectangle){230, 110, 30, 25}, "-", DARKGRAY) && process_count > 1) process_count--;
    DrawText(TextFormat("%d", process_count), 275, 115, 16, YELLOW);
    if (DrawButton((Rectangle){300, 110, 30, 25}, "+", DARKGRAY) && process_count < MAX_PROCESSES) process_count++;
    
    DrawText("RR Quantum (sec):", 380, 115, 16, WHITE);
    DrawNumericInput((Rectangle){530, 110, 60, 25}, &time_slice, 999);
    if (time_slice < 0.1f) time_slice = 0.1f; // Sanity check bound
    
    // Table Headers for the batch tasks
    int startY = 165;
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
        DrawNumericInput((Rectangle){130, rowY - 4, 80, 24}, &processes[i].arrival_time, (i * 3) + 0);
        DrawNumericInput((Rectangle){300, rowY - 4, 80, 24}, &processes[i].burst_time, (i * 3) + 1);
        DrawNumericInput((Rectangle){470, rowY - 4, 80, 24}, &processes[i].deadline, (i * 3) + 2);
        
        DrawRectangle(660, rowY - 2, 45, 20, processes[i].color);
    }
    
    DrawText("Tip: Click values to edit. Press [Enter] to submit text fields.", 40, 415, 13, GRAY);
    
    if (DrawButton((Rectangle){620, 100, 140, 40}, "LAUNCH SIMULATION", GREEN)) {
        active_input_field = -1; // clear active selections
        StartSimulation();
    }
}

void DrawProcesses() {
    for (int i = 0; i < process_count; i++) {
        int y = 60 + i * 36;

        DrawText(TextFormat("P%d", processes[i].id), 10, y, 18, WHITE);
        DrawRectangle(100, y, 200, 20, DARKGRAY);

        // Prevent division by zero if burst was configured to 0 manually
        float progress = 0.0f;
        if (processes[i].burst_time > 0) {
            progress = 1.0f - (processes[i].remaining_time / processes[i].burst_time);
        }
        
        DrawRectangle(100, y, (int)(200 * progress), 20, processes[i].color);
        DrawText(TextFormat("Arr: %.1f  Rem: %.1f  D: %.1f", 
                 processes[i].arrival_time, processes[i].remaining_time, processes[i].deadline), 
                 320, y, 14, LIGHTGRAY);
    }
}

void DrawCPU() {
    DrawRectangleLines(40, 400, 720, 45, GRAY);
    DrawText("CPU Core Allocation:", 55, 412, 16, LIGHTGRAY);

    if (current_process != -1) {
        DrawRectangle(250, 408, 120, 28, processes[current_process].color);
        DrawText(TextFormat("PROCESS P%d", current_process), 265, 414, 14, BLACK);
    } else {
        DrawText("IDLE (No active thread)", 250, 412, 16, DARKGRAY);
    }
}

int main() {
    InitWindow(800, 480, "Process Scheduler Simulator Architecture");
    SetTargetFPS(60);

    InitProcessesDefaults();

    while (!WindowShouldClose()) {
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
        ClearBackground((Color){20, 20, 25, 255});

        if (current_state == STATE_MENU) {
            DrawMenuPage();
        } 
        else if (current_state == STATE_RUNNING) {
            DrawText("1:FIFO  2:RR  3:EDF  4:CTS  [Esc]:Config Menu", 10, 15, 16, LIGHTGRAY);
            
            const char* schedNames[] = { "First-In First-Out", "Round Robin", "Earliest Deadline First", "Completely Fair (CTS)" };
            DrawText(TextFormat("Active Core Engine: %s", schedNames[scheduler]), 10, 38, 14, YELLOW);
            
            if (scheduler == RR) {
                DrawText(TextFormat("Quantum: %.1fs | Slice Timer: %.1fs", time_slice, rr_timer), 400, 38, 14, RAYWHITE);
            }

            DrawText(TextFormat("Global Clock: %.2fs", current_time), 630, 15, 16, GREEN);

            DrawProcesses();
            DrawCPU();
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}