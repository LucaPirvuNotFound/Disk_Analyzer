#include "structura.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

void handle_server_response(int fd, CommandType type) {
  IPCResponse res;
  // Citim răspunsul de bază
  if (read(fd, &res, sizeof(res)) <= 0)
    return;

  if (!res.success) {
    printf("%s\n", res.message);
    return;
  }

  // Gestionăm afișarea în funcție de ce am cerut
  if (type == CMD_ADD) {
    printf("Created analysis task with ID '%d' for '%s'.\n", res.job_id,
           res.message);
  } 
  else if (type == CMD_LIST) {
    printf("%-2s %-3s %-20s %-4s %-12s %-s\n",
           "ID", "PRI", "Path", "Done", "Status", "Details");
    printf("---------------------------------------------------------------------\n");

    bool finished = false;

    while (!finished) {
        if (res.is_last) {
            finished = true;
        } else {
            printf("%-2d %s\n", res.job_id, res.message);

            if (read(fd, &res, sizeof(res)) <= 0)
                break;
        }
    }
}
  else if (type == CMD_PRINT) {
    printf("%-30s %-7s %-10s %-40s\n", "Path", "Usage", "Size", "Amount");
    printf("-------------------------------------------------------------------"
           "-------------\n");

    // Citim nodurile transmise de Daemon unul cate unul
    // Respunul initial a fost deja citit in 'res' la inceputul functiei
    // Daca Daemon-ul a trimis deja primul nod in primul 'read', il procesam,
    // apoi intram in loop

    bool finished = false;
    while (1) {
      // Desenam indentarea pe baza adancimii (depth)
      for (int i = 0; i < res.depth; i++) {
        printf("  |");
      }
      if (res.depth > 0)
        printf("-");

      // Afisam numele directorului
      printf("/%s/", res.dir_name);

      // Calculam spatierea pentru coloana de procente
      int current_pos = (res.depth * 3) + strlen(res.dir_name) + 2;
      int padding = 31 - current_pos;
      if (padding < 0)
        padding = 1;
      printf("%*s", padding, "");

      // Afisam procentul si marimea in MB
      printf("%5.1f%% ", res.percentage);
      printf("%-10.2fMB ", (double)res.dir_size / (1024 * 1024));

      // Desenam bara de progres cu '#'
      int num_hashes = (int)(res.percentage / 2.5); // 100% = 40 de '#'
      for (int i = 0; i < num_hashes; i++)
        printf("#");
      printf("\n");

      if (res.is_last) {
        break;
      } else {
        // Citim urmatorul nod
        if (read(fd, &res, sizeof(res)) <= 0)
          break;
      }
    }
  } else {
    printf("%s\n", res.message);
  }
}

void send_request(IPCRequest req) {
  int fd;
  struct sockaddr_un addr;

  if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket error");
    exit(1);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("connect error (este daemonul pornit?)");
    exit(1);
  }

  write(fd, &req, sizeof(req));

  handle_server_response(fd, req.type);

  close(fd);
}

int main(int argc, char *argv[]) {
  int opt;
  IPCRequest req;
  memset(&req, 0, sizeof(req));
  req.priority = 2; // Default normal

  static struct option long_options[] = {
      {"add", required_argument, 0, 'a'},
      {"priority", required_argument, 0, 'p'},
      {"suspend", required_argument, 0, 'S'},
      {"resume", required_argument, 0, 'R'},
      {"remove", required_argument, 0, 'r'},
      {"info", required_argument, 0, 'i'},
      {"list", no_argument, 0, 'l'},
      {"print", required_argument, 0, 'P'},
      {0, 0, 0, 0}};

  while ((opt = getopt_long(argc, argv, "a:p:S:R:r:i:lP:", long_options,
                            NULL)) != -1) {
    switch (opt) {
    case 'a':
      req.type = CMD_ADD;
      realpath(optarg, req.path); // Convertim în cale absolută
      break;
    case 'p':
      req.priority = atoi(optarg);
      break;
    case 'S':
      req.type = CMD_SUSPEND;
      req.job_id = atoi(optarg);
      break;
    case 'R':
      req.type = CMD_RESUME;
      req.job_id = atoi(optarg);
      break;
    case 'r':
    req.type = CMD_REMOVE;
    req.job_id = atoi(optarg);
    break;
    case 'i':
      req.type = CMD_INFO;
      req.job_id = atoi(optarg);
      break;
    case 'l':
      req.type = CMD_LIST;
      break;
    case 'P':
      req.type = CMD_PRINT;
      req.job_id = atoi(optarg);
      break;
    }
  }

  send_request(req);
  return 0;
}
