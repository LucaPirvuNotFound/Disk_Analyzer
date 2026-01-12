#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "structura.h"

#define NR_JOBS 100

// Functie pentru transformarea procesului in Daemon
void make_daemon() {
    pid_t pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0); // Parintele moare

    if (setsid() < 0) exit(1); // Devine lider de sesiune

    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0); // Al doilea parinte moare

    umask(0);
    chdir("/");

    // Inchidem descriptorii standard (stdin, stdout, stderr)
    // Un daemon nu are voie sa scrie in terminalul care l-a lansat
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

}

// Returnează true dacă path_to_check este deja gestionat de un job existent
bool is_already_analyzed(const char *path_to_check, AnalysisJob *existing_jobs, int num_jobs) {
    for (int i = 0; i < num_jobs; i++) {
        if (existing_jobs[i].status == REMOVED) continue;

        char *existing = existing_jobs[i].root_path;

        // Cazul 1: Căile sunt identice
        if (strcmp(path_to_check, existing) == 0) return true;

        // Cazul 2: path_to_check este un sub-director al unui job existent
        // Exemplu: existent="/home/user", nou="/home/user/descarcari"
        int len_existing = strlen(existing);
        if (strncmp(path_to_check, existing, len_existing) == 0) {
            if (path_to_check[len_existing] == '/') return true;
        }

        // Cazul 3: Job-ul existent este un sub-director al noului path
        // (Opțional, depinde de interpretare: dacă adaugi /home, iar /home/user e deja acolo, 
        // noul job îl va include pe cel vechi, deci cel vechi devine redundant)
        int len_check = strlen(path_to_check);
        if (strncmp(existing, path_to_check, len_check) == 0) {
            if (existing[len_check] == '/') return true;
        }
    }
    return false;
}



// Funcție helper în Daemon
void send_tree_recursive(int client_fd, DirNode *node, int depth, long total_job_size) {
    if (node == NULL) return;

    IPCResponse res;
    memset(&res, 0, sizeof(res));
    res.success = true;
    res.is_last = false;
    res.depth = depth;
    res.dir_size = node->size;
    strncpy(res.dir_name, node->name, 255);
    
    // Calculăm cât la sută reprezintă acest director din rădăcină
    res.percentage = (total_job_size > 0) ? ((float)node->size / total_job_size * 100.0) : 0;

    // Trimitem nodul curent
    write(client_fd, &res, sizeof(res));

    // Mergem la primul copil (coborâm în ierarhie)
    send_tree_recursive(client_fd, node->children, depth + 1, total_job_size);

    // Mergem la următorul frate (pe același nivel)
    send_tree_recursive(client_fd, node->next_sibling, depth, total_job_size);
}



int main() {
    // APELAM FUNCTIA DE DAEMONIZARE
    make_daemon();

    int server_fd, client_fd;
    struct sockaddr_un addr;
    AnalysisJob lista_joburi[NR_JOBS]; // Simulam o lista de joburi
    int job_count = 0;

    // Creare socket
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(SOCKET_PATH); // Stergem socket-ul vechi daca exista

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Daemon pornit si asteapta comenzi...\n");

    while ((client_fd = accept(server_fd, NULL, NULL)) != -1) {
        IPCRequest req;
        read(client_fd, &req, sizeof(req));

        IPCResponse res;
        memset(&res, 0, sizeof(res));

        if (req.type == CMD_ADD) {
            if (is_already_analyzed(req.path, lista_joburi, job_count)) {
                res.success = false;
                sprintf(res.message, "Directorul '%s' este deja inclus in analiza.", req.path);
            } else {
                res.success = true;
                res.job_id = ++job_count;
                strcpy(lista_joburi[job_count-1].root_path, req.path);
                lista_joburi[job_count-1].status = PROGRESS;
                sprintf(res.message, "%s", req.path);
            }
            write(client_fd, &res, sizeof(res));
        }
        else if (req.type == CMD_PRINT) {
            // Simulam un arbore pentru testarea print-ului
            DirNode root = {.size = 104857600, .name = "root"}; // 100MB
            DirNode c1 = {.size = 31457280, .name = "src", .parent = &root}; // 30MB
            DirNode c2 = {.size = 52428800, .name = "bin", .parent = &root}; // 50MB
            root.children = &c1;
            c1.next_sibling = &c2;

            // Trimitem succesul initial
            IPCResponse start_res = {.success = true};
            write(client_fd, &start_res, sizeof(start_res));

            // Trimitem arborele
            send_tree_recursive(client_fd, &root, 0, root.size);

            // Trimitem pachetul de final
            IPCResponse end_res = {.is_last = true};
            write(client_fd, &end_res, sizeof(end_res));
        }
        
        close(client_fd);
    }
    return 0;
}