#ifndef structure_h
#define structure_h

#include <limits.h>
#include <stdbool.h>

#define SOCKET_PATH "/tmp/diskanalyzer.sock"

typedef enum { PENDING, PROGRESS, PAUSED, DONE, REMOVED } JobStatus;

// Structura pentru un Director (e un arbore, dar care are acces la nodurile care au acelasi tata ca el si la copii)
typedef struct DirNode {
    char name[256];
    long size;              // Dimensiunea totala (suma tuturor fisierelor din el)
    struct DirNode *parent;
    struct DirNode *children;
    struct DirNode *next_sibling;
} DirNode;

// Structura pentru un job de analiza
typedef struct {
    int id;
    char root_path[PATH_MAX];
    int priority;           // 1-low, 2-normal, 3-high
    JobStatus status;
    DirNode *root_node;     // Radacina arborelui de directoare
} AnalysisJob;

// Protocol de comunicare IPC (practic astea sunt comenzile pe care trebuie sa le implementam, iar structura asta o completeaza utilizatorul cand introduce comanda folosind utilitarul "da")
typedef enum { CMD_ADD, CMD_SUSPEND, CMD_RESUME, CMD_REMOVE, CMD_INFO, CMD_LIST, CMD_PRINT } CommandType;

typedef struct {
    CommandType type;
    int job_id;
    int priority;
    char path[PATH_MAX];
} IPCRequest;

#endif