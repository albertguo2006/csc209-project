#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "protocol.h"

#ifndef PORT
#define PORT 4242
#endif

/* =========================================================================
 * Client state
 * ========================================================================= */

static int       my_id        = -1;
static int       action_count = 0;
static ActionDef actions[MAX_ACTIONS];

/* Receive buffer for partial message reassembly */
static uint8_t   rbuf[4096];
static int       rbuf_len = 0;

/* Set when we've submitted our action and are waiting */
static int waiting_for_result = 0;

/* =========================================================================
 * Utility
 * ========================================================================= */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t rem = len;
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n <= 0) return -1;
        p   += n;
        rem -= (size_t)n;
    }
    return 0;
}

static void clear_line(void)
{
    printf("\r\033[K"); /* Move to column 0, erase to end of line */
    fflush(stdout);
}

static void print_hp_bar(int hp)
{
    int filled = hp / 5; /* 20 chars = full bar (100 hp) */
    printf("[");
    for (int i = 0; i < 20; i++) {
        printf(i < filled ? "#" : ".");
    }
    printf("] %3d HP", hp);
}

/* =========================================================================
 * Message handlers
 * ========================================================================= */

static void on_lobby_update(const LobbyUpdateMsg *m)
{
    printf("\n=== Lobby (%d player%s) ===\n",
           m->player_count, m->player_count == 1 ? "" : "s");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (m->players[i].name[0] != '\0') {
            printf("  [%d] %s\n", m->players[i].player_id, m->players[i].name);
        }
    }
    printf("Waiting for more players...\n");
    fflush(stdout);
}

static void on_game_start(const GameStartMsg *m)
{
    my_id        = m->your_id;
    action_count = m->action_count;
    for (int i = 0; i < m->action_count; i++) {
        actions[i] = m->actions[i];
    }

    printf("\n\033[1m=== GAME START ===\033[0m\n");
    printf("You are Player %d\n\n", my_id);
    printf("Players:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (m->players[i].name[0] == '\0') continue;
        printf("  [%d] %s", m->players[i].player_id, m->players[i].name);
        if (m->players[i].player_id == (uint8_t)my_id) printf(" (YOU)");
        printf("\n");
    }
    printf("\nAvailable actions:\n");
    for (int i = 0; i < m->action_count; i++) {
        printf("  %d) %-10s — %s\n",
               m->actions[i].action_id,
               m->actions[i].name,
               m->actions[i].desc);
    }
    fflush(stdout);
}

static void on_round_start(const RoundStartMsg *m)
{
    waiting_for_result = 0;
    printf("\n\033[1m--- Round %d ---\033[0m  (timer: %ds)\n", m->round_num, m->timer_secs);
    printf("Current HP:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (m->players[i].name[0] == '\0') continue;
        printf("  [%d] %-20s ", m->players[i].player_id, m->players[i].name);
        if (m->players[i].alive) {
            print_hp_bar(m->players[i].hp);
        } else {
            printf("ELIMINATED");
        }
        if (m->players[i].player_id == (uint8_t)my_id) printf("  <-- YOU");
        printf("\n");
    }
    fflush(stdout);
}

static void on_timer_tick(const TimerTickMsg *m)
{
    clear_line();
    printf("Time remaining: %2ds", m->seconds_left);
    fflush(stdout);
}

static void on_round_result(const RoundResultMsg *m)
{
    printf("\n\n\033[1m=== Round Results ===\033[0m\n");
    for (int i = 0; i < m->result_count; i++) {
        const ActionResult *r = &m->results[i];
        /* Find actor name */
        const char *aname = "?";
        const char *tname = "?";
        for (int j = 0; j < MAX_PLAYERS; j++) {
            if (m->players[j].player_id == r->actor_id)  aname = m->players[j].name;
            if (m->players[j].player_id == r->target_id) tname = m->players[j].name;
        }
        /* Find action name */
        const char *action_name = "?";
        for (int j = 0; j < action_count; j++) {
            if (actions[j].action_id == r->action_id) {
                action_name = actions[j].name;
                break;
            }
        }

        if (r->target_id == NO_TARGET) {
            printf("  %s used %s", aname, action_name);
            if (r->hp_delta > 0) printf(" (+%d HP)", r->hp_delta);
            printf("\n");
        } else {
            printf("  %s -> %s: %s", aname, tname, action_name);
            if (r->hp_delta < 0) printf(" (%d HP)", r->hp_delta);
            printf("\n");
        }
    }

    printf("\nHP after round:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (m->players[i].name[0] == '\0') continue;
        printf("  [%d] %-20s ", m->players[i].player_id, m->players[i].name);
        if (m->players[i].alive) {
            print_hp_bar(m->players[i].hp);
        } else {
            printf("ELIMINATED");
        }
        printf("\n");
    }
    fflush(stdout);
}

static void on_player_elim(const PlayerElimMsg *m)
{
    printf("\n\033[31m*** %s has been eliminated! ***\033[0m\n", m->name);
    fflush(stdout);
}

static void on_game_over(const GameOverMsg *m)
{
    printf("\n\033[1m=== GAME OVER ===\033[0m\n");
    if (m->is_tie) {
        printf("It's a TIE! Multiple players were eliminated simultaneously.\n");
    } else {
        printf("\033[33m%s wins!\033[0m\n", m->winner_name);
        if (m->winner_id == (uint8_t)my_id) {
            printf("\033[1mCongratulations, YOU WIN!\033[0m\n");
        } else {
            printf("Better luck next time.\n");
        }
    }
    fflush(stdout);
}

static void on_error(const ErrorMsg *m)
{
    printf("\n\033[31m[Error] %s\033[0m\n", m->message);
    fflush(stdout);
}

static void on_wait(void)
{
    waiting_for_result = 1;
    printf("\nAction submitted. Waiting for other players...\n");
    fflush(stdout);
}

/* =========================================================================
 * Process bytes in rbuf, dispatch complete messages
 * ========================================================================= */

static int process_server_buffer(void)
{
    while (rbuf_len > 0) {
        uint8_t opcode = rbuf[0];
        size_t needed  = 0;

        switch (opcode) {
            case MSG_LOBBY_UPDATE: needed = sizeof(LobbyUpdateMsg);  break;
            case MSG_GAME_START:   needed = sizeof(GameStartMsg);    break;
            case MSG_ROUND_START:  needed = sizeof(RoundStartMsg);   break;
            case MSG_TIMER_TICK:   needed = sizeof(TimerTickMsg);    break;
            case MSG_ROUND_RESULT: needed = sizeof(RoundResultMsg);  break;
            case MSG_PLAYER_ELIM:  needed = sizeof(PlayerElimMsg);   break;
            case MSG_GAME_OVER:    needed = sizeof(GameOverMsg);     break;
            case MSG_ERROR:        needed = sizeof(ErrorMsg);        break;
            case MSG_WAIT:         needed = sizeof(WaitMsg);         break;
            default:
                fprintf(stderr, "Unknown opcode 0x%02x from server\n", opcode);
                return -1;
        }

        if ((size_t)rbuf_len < needed) break;

        switch (opcode) {
            case MSG_LOBBY_UPDATE: on_lobby_update((const LobbyUpdateMsg *)rbuf); break;
            case MSG_GAME_START:   on_game_start((const GameStartMsg *)rbuf);     break;
            case MSG_ROUND_START:  on_round_start((const RoundStartMsg *)rbuf);   break;
            case MSG_TIMER_TICK:   on_timer_tick((const TimerTickMsg *)rbuf);     break;
            case MSG_ROUND_RESULT: on_round_result((const RoundResultMsg *)rbuf); break;
            case MSG_PLAYER_ELIM:  on_player_elim((const PlayerElimMsg *)rbuf);   break;
            case MSG_GAME_OVER:    on_game_over((const GameOverMsg *)rbuf);        break;
            case MSG_ERROR:        on_error((const ErrorMsg *)rbuf);              break;
            case MSG_WAIT:         on_wait();                                     break;
        }

        int rem = rbuf_len - (int)needed;
        if (rem > 0) memmove(rbuf, rbuf + needed, (size_t)rem);
        rbuf_len = rem;
    }
    return 0;
}

/* =========================================================================
 * Prompt and send an action
 * ========================================================================= */

static void prompt_action(int sockfd)
{
    (void)sockfd;
    if (my_id < 0 || waiting_for_result) return;

    printf("\nYour turn! Choose an action:\n");
    for (int i = 0; i < action_count; i++) {
        printf("  %d) %-10s — %s\n",
               actions[i].action_id,
               actions[i].name,
               actions[i].desc);
    }
    printf("> ");
    fflush(stdout);
}

static void handle_stdin(int sockfd)
{
    char line[128];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        printf("EOF on stdin, disconnecting.\n");
        close(sockfd);
        exit(0);
    }

    /* Strip newline */
    line[strcspn(line, "\n")] = '\0';

    if (my_id < 0) {
        /* Should not happen after game start, but guard anyway */
        return;
    }

    /* Parse: "<action_id> [target_id]" */
    int aid = -1, tid = NO_TARGET;
    int parsed = sscanf(line, "%d %d", &aid, &tid);

    if (parsed < 1 || aid < 0 || aid >= action_count) {
        printf("Enter action number (0-%d), optionally followed by target player ID.\n",
               action_count - 1);
        prompt_action(sockfd);
        return;
    }

    if (actions[aid].requires_target && parsed < 2) {
        printf("Action '%s' requires a target. Usage: %d <target_id>\n",
               actions[aid].name, aid);
        prompt_action(sockfd);
        return;
    }

    ActionMsg m;
    m.msg_type  = MSG_ACTION;
    m.action_id = (uint8_t)aid;
    m.target_id = (uint8_t)(parsed >= 2 ? tid : NO_TARGET);
    m.padding   = 0;

    if (write_all(sockfd, &m, sizeof(m)) < 0) {
        perror("write");
        exit(1);
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int port = PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    /* Get player name */
    char name[PLAYER_NAME_LEN];
    printf("Enter your name: ");
    fflush(stdout);
    if (fgets(name, sizeof(name), stdin) == NULL) {
        fprintf(stderr, "No name provided.\n");
        return 1;
    }
    name[strcspn(name, "\n")] = '\0';
    if (name[0] == '\0') {
        fprintf(stderr, "Name cannot be empty.\n");
        return 1;
    }

    /* Connect */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected to %s:%d\n", host, port);

    /* Send join message */
    JoinMsg jm;
    memset(&jm, 0, sizeof(jm));
    jm.msg_type = MSG_JOIN;
    strncpy(jm.name, name, PLAYER_NAME_LEN - 1);
    if (write_all(sockfd, &jm, sizeof(jm)) < 0) {
        perror("write");
        return 1;
    }

    /* Event loop: select on sockfd + stdin */
    int game_over = 0;
    int need_prompt = 0;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;

        int ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Read from server */
        if (FD_ISSET(sockfd, &rfds)) {
            int space = (int)sizeof(rbuf) - rbuf_len;
            ssize_t n = read(sockfd, rbuf + rbuf_len, (size_t)space);
            if (n <= 0) {
                printf("\nServer closed connection.\n");
                break;
            }
            rbuf_len += (int)n;
            if (process_server_buffer() < 0) break;

            /* If we just got a round start and aren't waiting, prompt */
            if (!waiting_for_result && my_id >= 0 && !game_over) {
                need_prompt = 1;
            }
            if (rbuf[0] == MSG_GAME_OVER || game_over) {
                game_over = 1;
                need_prompt = 0;
            }
        }

        /* Read from stdin */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (my_id >= 0 && !waiting_for_result && !game_over) {
                handle_stdin(sockfd);
                need_prompt = 0;
            } else {
                /* Drain stdin */
                char tmp[128];
                if (fgets(tmp, sizeof(tmp), stdin) == NULL) break;
            }
        }

        if (need_prompt) {
            prompt_action(sockfd);
            need_prompt = 0;
        }
    }

    close(sockfd);
    return 0;
}
