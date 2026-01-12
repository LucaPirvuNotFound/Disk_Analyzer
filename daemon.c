#include "structura.h"
#include <dirent.h>  // Necesar pentru Task 2 (opendir)
#include <libgen.h>  // at the top of daemon.c
#include <pthread.h> // Necesar pentru Task 3 (Threads)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h> // Necesar pentru Task 2 (stat)
#include <sys/un.h>
#include <unistd.h>

#define NR_JOBS 100

// Manage de prioritati
AnalysisJob *priority_queues[3][NR_JOBS]; // 0: Low, 1: Normal, 2: High
int queue_counts[3] = {0, 0, 0};
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_available = PTHREAD_COND_INITIALIZER;

// --- Structura de date
DirNode *create_node(const char *name, DirNode *parent) {
  DirNode *node = (DirNode *)calloc(1, sizeof(DirNode));
  if (!node)
    return NULL;
  strncpy(node->name, name, 255);
  node->parent = parent;
  return node;
}

void add_child(DirNode *parent, DirNode *child) {
  if (!parent->children) {
    parent->children = child;
  } else {
    DirNode *temp = parent->children;
    while (temp->next_sibling)
      temp = temp->next_sibling;
    temp->next_sibling = child;
  }
}

void count_nodes_recursive(DirNode *node, long *files, long *dirs) {
  if (!node)
    return;

  (*dirs)++;
  (*files) += node->file_count;

  count_nodes_recursive(node->children, files, dirs);
  count_nodes_recursive(node->next_sibling, files, dirs);
}

//  Crawler-ul Recursiv ---
long crawl_recursive(const char *path, DirNode *parent_node, AnalysisJob *job) {
  // Verificare pentru Suspend/Resume
  while (1) {
    if (job->status == REMOVED)
      return 0;
    if (job->status == PROGRESS)
      break;
    sleep(1); // Staționează dacă este în PAUSED sau PENDING
  }

  DIR *dir = opendir(path);
  if (!dir)
    return 0;

  struct dirent *entry;
  long total_size = 0;

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

    struct stat st;
    if (stat(full_path, &st) == -1)
      continue;

    if (S_ISDIR(st.st_mode)) {
      DirNode *child = create_node(entry->d_name, parent_node);
      add_child(parent_node, child);
      child->size = crawl_recursive(full_path, child, job);
      total_size += child->size;
      parent_node->file_count += child->file_count; 
    } else {
      total_size += st.st_size;
      parent_node->file_count++; 
    }
  }
  closedir(dir);
  return total_size;
}

//  Worker Thread Function ---
void *worker_thread_func(void *arg) {
  while (1) {
    AnalysisJob *current_job = NULL;
    pthread_mutex_lock(&queue_mutex);

    while (queue_counts[0] == 0 && queue_counts[1] == 0 &&
           queue_counts[2] == 0) {
      pthread_cond_wait(&job_available, &queue_mutex);
    }

    // Extrage job-ul cu prioritatea cea mai mare (3 -> index 2)
    for (int i = 2; i >= 0; i--) {
      if (queue_counts[i] > 0) {
        current_job = priority_queues[i][0];
        for (int j = 0; j < queue_counts[i] - 1; j++)
          priority_queues[i][j] = priority_queues[i][j + 1];
        queue_counts[i]--;
        break;
      }
    }
    pthread_mutex_unlock(&queue_mutex);

    if (current_job) {
      current_job->status = PROGRESS;

      char path_copy[PATH_MAX];
      strncpy(path_copy, current_job->root_path, PATH_MAX);
      current_job->root_node = create_node(basename(path_copy), NULL);
      fprintf(stderr, "DEBUG: Root node created with name='%s' for path='%s'\n",
              current_job->root_node->name, current_job->root_path);

      current_job->root_node->size = crawl_recursive(
          current_job->root_path, current_job->root_node, current_job);

      if (current_job->status != REMOVED)
        current_job->status = DONE;
    }
  }
  return NULL;
}

void enqueue_job(AnalysisJob *job) {
  pthread_mutex_lock(&queue_mutex);
  int idx = job->priority - 1; // 1-low (0), 2-normal (1), 3-high (2)
  priority_queues[idx][queue_counts[idx]++] = job;
  pthread_cond_signal(&job_available);
  pthread_mutex_unlock(&queue_mutex);
}

// Functie pentru transformarea procesului in Daemon
void make_daemon() {
  pid_t pid = fork();
  if (pid < 0)
    exit(1);
  if (pid > 0)
    exit(0); // Parintele moare

  if (setsid() < 0)
    exit(1); // Devine lider de sesiune

  pid = fork();
  if (pid < 0)
    exit(1);
  if (pid > 0)
    exit(0); // Al doilea parinte moare

  umask(0);
  chdir("/");

  // Inchidem descriptorii standard (stdin, stdout, stderr)
  // Un daemon nu are voie sa scrie in terminalul care l-a lansat
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
}

// Returnează true dacă path_to_check este deja gestionat de un job existent
bool is_already_analyzed(const char *path_to_check, AnalysisJob *existing_jobs,
                         int num_jobs) {
  for (int i = 0; i < num_jobs; i++) {
    if (existing_jobs[i].status == REMOVED)
      continue;

    char *existing = existing_jobs[i].root_path;

    // Cazul 1: Căile sunt identice
    if (strcmp(path_to_check, existing) == 0)
      return true;

    // Cazul 2: path_to_check este un sub-director al unui job existent
    // Exemplu: existent="/home/user", nou="/home/user/descarcari"
    int len_existing = strlen(existing);
    if (strncmp(path_to_check, existing, len_existing) == 0) {
      if (path_to_check[len_existing] == '/')
        return true;
    }

    // Cazul 3: Job-ul existent este un sub-director al noului path
    int len_check = strlen(path_to_check);
    if (strncmp(existing, path_to_check, len_check) == 0) {
      if (existing[len_check] == '/')
        return true;
    }
  }
  return false;
}

// Funcție helper în Daemon
void send_tree_recursive(int client_fd, DirNode *node, int depth,
                         long total_job_size) {
  if (node == NULL)
    return;

  IPCResponse res;
  memset(&res, 0, sizeof(res));
  res.success = true;
  res.is_last = false;
  res.depth = depth;
  res.dir_size = node->size;
  strncpy(res.dir_name, node->name, 255);

  // Calculăm cât la sută reprezintă acest director din rădăcină
  res.percentage =
      (total_job_size > 0) ? ((float)node->size / total_job_size * 100.0) : 0;

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

  // Initializare Thread Worker pentru analiza
  pthread_t worker_tid;
  pthread_create(&worker_tid, NULL, worker_thread_func, NULL);

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

  bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
  listen(server_fd, 5);

  // Nota: printf nu va aparea in terminal dupa make_daemon()

  while ((client_fd = accept(server_fd, NULL, NULL)) != -1) {
    IPCRequest req;
    read(client_fd, &req, sizeof(req));

    IPCResponse res;
    memset(&res, 0, sizeof(res));

    if (req.type == CMD_ADD) {
      if (is_already_analyzed(req.path, lista_joburi, job_count)) {
        res.success = false;
        sprintf(res.message, "Directorul '%s' este deja inclus in analiza.",
                req.path);
      } else {
        res.success = true;
        res.job_id = ++job_count;

        AnalysisJob *new_job = &lista_joburi[job_count - 1];
        new_job->id = res.job_id;
        strcpy(new_job->root_path, req.path);
        new_job->priority = req.priority;
        new_job->status = PENDING;

        enqueue_job(new_job); // Adaugam in coada de prioritati
        sprintf(res.message, "%s", req.path);
      }
      write(client_fd, &res, sizeof(res));
    } else if (req.type == CMD_SUSPEND || req.type == CMD_RESUME ||
               req.type == CMD_REMOVE) {
      for (int i = 0; i < job_count; i++) {
        if (lista_joburi[i].id == req.job_id) {
          if (req.type == CMD_SUSPEND)
            lista_joburi[i].status = PAUSED;
          else if (req.type == CMD_RESUME)
            lista_joburi[i].status = PROGRESS;
          else if (req.type == CMD_REMOVE)
            lista_joburi[i].status = REMOVED;
          res.success = true;
          break;
        }
      }
      write(client_fd, &res, sizeof(res));
    } else if (req.type == CMD_LIST) {
      for (int i = 0; i < job_count; i++) {
        if (lista_joburi[i].status == REMOVED)
          continue;

        long files = 0;
        long dirs = 0;

        if (lista_joburi[i].root_node) {
          count_nodes_recursive(lista_joburi[i].root_node, &files, &dirs);
        }

        IPCResponse res_job;
        memset(&res_job, 0, sizeof(res_job));

        res_job.success = true; // 🔴 THIS WAS MISSING
        res_job.job_id = lista_joburi[i].id;

        char stars[4] = "";
        for (int p = 0; p < lista_joburi[i].priority && p < 3; p++)
          strcat(stars, "*");

        const char *status_txt = (lista_joburi[i].status == PROGRESS)
                                     ? "in progress"
                                 : (lista_joburi[i].status == DONE) ? "done"
                                                                    : "paused";

        sprintf(res_job.message, "%-3s %-20s %d%% %-12s %ld files, %ld dirs",
                stars, lista_joburi[i].root_path,
                (lista_joburi[i].status == DONE ? 100 : 45), status_txt, files,
                dirs);

        write(client_fd, &res_job, sizeof(res_job));
      }

      IPCResponse last_res;
      memset(&last_res, 0, sizeof(last_res));
      last_res.success = true; // 🔴 ALSO REQUIRED
      last_res.is_last = true;
      write(client_fd, &last_res, sizeof(last_res));
    }
    // Inside the while loop in daemon.c
    else if (req.type == CMD_PRINT) {
      // Găsim job-ul pentru job_id
      AnalysisJob *job_to_print = NULL;
      for (int i = 0; i < job_count; i++) {
        if (lista_joburi[i].id == req.job_id) {
          job_to_print = &lista_joburi[i];
          break;
        }
      }
      if (!job_to_print || !job_to_print->root_node) {
        IPCResponse res = {.success = false};
        sprintf(res.message, "Job not found or not ready.");
        write(client_fd, &res, sizeof(res));
        continue;
      }

      // Trimitem pachetul de start
      IPCResponse start_res = {.success = true};
      write(client_fd, &start_res, sizeof(start_res));

      // Trimitem arborele nod cu nod
      send_tree_recursive(client_fd, job_to_print->root_node, 0,
                          job_to_print->root_node->size);

      // Trimitem pachetul de final
      IPCResponse end_res = {.is_last = true};
      write(client_fd, &end_res, sizeof(end_res));
    }

    close(client_fd);
  }
  return 0;
}
