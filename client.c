#define _DEFAULT_SOURCE  /* for usleep() */
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
static int       my_char_id   = -1;
static int       action_count = 0;
static ActionDef actions[MOVES_PER_CHAR];

static uint8_t   rbuf[8192];
static int       rbuf_len = 0;

static int waiting_for_result = 0;
static int need_prompt        = 0;
static int game_over          = 0;
static int am_host            = 0;
static int my_lobby_id        = -1;
static int in_resolution      = 0;  /* 1 while watching round events */
static int char_selected      = 0;  /* 1 if character picked */
static int timer_line_displayed = 0;

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
    printf("\r\033[K");
    fflush(stdout);
}

static void print_hp_bar(int hp, int max_hp)
{
    if (max_hp <= 0) max_hp = 100;
    int filled = hp * 20 / max_hp;
    if (filled < 0) filled = 0;
    if (filled > 20) filled = 20;
    printf("[");
    for (int i = 0; i < 20; i++)
        printf(i < filled ? "#" : ".");
    printf("] %3d/%d HP", hp, max_hp);
}

/* Typewriter effect: print text character by character */
static void typewriter_print(const char *text)
{
    for (int i = 0; text[i]; i++) {
        putchar(text[i]);
        fflush(stdout);
        usleep(15000); /* 15ms per character */
    }
    printf("\n");
    fflush(stdout);
}

/* =========================================================================
 * Message handlers
 * ========================================================================= */

static void on_lobby_update(const LobbyUpdateMsg *m)
{
    printf("\n\033[1m=== Lobby (%d player%s) ===\033[0m\n",
           m->player_count, m->player_count == 1 ? "" : "s");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (m->players[i].name[0] == '\0') continue;
        printf("  [%d] %-20s", m->players[i].player_id, m->players[i].name);
        if (m->players[i].char_id != NO_CHARACTER)
            printf("  (%s)", CHAR_TYPE_NAMES[m->players[i].char_type]);
        else
            printf("  (no character)");
        if (m->players[i].player_id == m->host_id) printf(" [HOST]");
        if (m->players[i].player_id == (uint8_t)my_lobby_id) printf(" [YOU]");
        printf("\n");
    }
    if (!char_selected)
        printf("\nType 'pick <0-7>' to select a character.\n");
    if (am_host)
        printf("Type 'start' when everyone is ready.\n");
    else if (char_selected)
        printf("Waiting for the host to start...\n");
    fflush(stdout);
}

static void on_join_ack(const JoinAckMsg *m)
{
    my_lobby_id = m->your_id;
    am_host     = m->is_host;
    if (am_host)
        printf("You are the host.\n");
    fflush(stdout);
}

static void on_char_list(const CharListMsg *m)
{
    printf("\n\033[1m=== Available Characters ===\033[0m\n");
    for (int i = 0; i < m->char_count; i++) {
        const CharEntry *c = &m->chars[i];
        printf("  %d) %-16s [%s]  HP:%d  Spd:%d",
               c->char_id, c->name,
               CHAR_TYPE_NAMES[c->char_type],
               c->hp, c->speed);
        if (c->taken) printf("  \033[31m(TAKEN)\033[0m");
        printf("\n");
        for (int j = 0; j < c->num_moves; j++) {
            printf("     %d: %-20s %s\n",
                   c->moves[j].action_id,
                   c->moves[j].name,
                   c->moves[j].desc);
        }
    }
    printf("\nType 'pick <number>' to select a character.\n");
    fflush(stdout);
}

static void on_game_start(const GameStartMsg *m)
{
    my_id        = m->your_id;
    action_count = m->action_count;
    for (int i = 0; i < m->action_count && i < MOVES_PER_CHAR; i++)
        actions[i] = m->actions[i];

    printf("\n\033[1m=== GAME START ===\033[0m\n");
    printf("You are Player %d\n\n", my_id);
    printf("Players:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (m->players[i].name[0] == '\0') continue;
        printf("  [%d] %-20s [%s] HP:%d Spd:%d",
               m->players[i].player_id,
               m->players[i].name,
               CHAR_TYPE_NAMES[m->players[i].char_type],
               m->players[i].hp,
               m->players[i].speed);
        if (m->players[i].player_id == (uint8_t)my_id) printf(" (YOU)");
        printf("\n");
    }
    printf("\nYour moves:\n");
    for (int i = 0; i < m->action_count; i++) {
        printf("  %d) %-20s -- %s",
               m->actions[i].action_id,
               m->actions[i].name,
               m->actions[i].desc);
        if (m->actions[i].requires_target == 1) printf("  [target enemy]");
        else if (m->actions[i].requires_target == 2) printf("  [target any]");
        printf("\n");
    }
    fflush(stdout);
}

static void on_round_start(const RoundStartMsg *m)
{
    waiting_for_result = 0;
    in_resolution = 0;
    need_prompt = 1;
    timer_line_displayed = 0;

    printf("\n\033[1m--- Round %d ---\033[0m  (timer: %ds)\n",
           m->round_num, m->timer_secs);
    printf("Current HP:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (m->players[i].name[0] == '\0') continue;
        printf("  [%d] %-20s ", m->players[i].player_id, m->players[i].name);
        if (m->players[i].alive)
            print_hp_bar(m->players[i].hp, m->players[i].max_hp);
        else
            printf("ELIMINATED");
        if (m->players[i].player_id == (uint8_t)my_id) printf("  <-- YOU");
        printf("\n");
    }
    fflush(stdout);
}

static void on_timer_tick(const TimerTickMsg *m)
{
    if (waiting_for_result) {
        clear_line();
        printf("Time remaining: %2ds | Waiting for other players...", m->seconds_left);
    } else if (timer_line_displayed) {
        printf("\0337\033[A\r\033[K"); 
        printf("Time remaining: %2ds", m->seconds_left);
        printf("\0338"); 
    } else {
        clear_line();
        printf("Time remaining: %2ds", m->seconds_left);
    }
    fflush(stdout);
}

static void on_round_event(const RoundEventMsg *m)
{
    in_resolution = 1;
    if (!waiting_for_result) {
        /* First event while still at prompt -- clear line */
        clear_line();
        waiting_for_result = 1;
    }
    printf("\n");
    typewriter_print(m->text);
}

static void on_status_update(const StatusUpdateMsg *m)
{
    printf("\033[33mStatus: %s\033[0m\n", m->text);
    fflush(stdout);
}

static void on_player_elim(const PlayerElimMsg *m)
{
    printf("\n\033[31m*** %s has been eliminated! ***\033[0m\n", m->name);
    fflush(stdout);
}

static void on_game_over(const GameOverMsg *m)
{
    game_over = 1;
    printf("\n\033[1m=== GAME OVER ===\033[0m\n");
    if (m->is_tie) {
        printf("It's a TIE! Multiple players eliminated simultaneously.\n");
    } else {
        printf("\033[33m%s wins!\033[0m\n", m->winner_name);
        if (m->winner_id == (uint8_t)my_id)
            printf("\033[1mCongratulations, YOU WIN!\033[0m\n");
        else
            printf("Better luck next time.\n");
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
    timer_line_displayed = 0;
    printf("\nAction submitted. Waiting for other players...\n");
    fflush(stdout);
}

/* =========================================================================
 * Process server buffer
 * ========================================================================= */

static int process_server_buffer(void)
{
    while (rbuf_len > 0) {
        uint8_t opcode = rbuf[0];
        size_t needed = 0;

        switch (opcode) {
        case MSG_LOBBY_UPDATE:  needed = sizeof(LobbyUpdateMsg);  break;
        case MSG_JOIN_ACK:      needed = sizeof(JoinAckMsg);      break;
        case MSG_GAME_START:    needed = sizeof(GameStartMsg);    break;
        case MSG_ROUND_START:   needed = sizeof(RoundStartMsg);   break;
        case MSG_TIMER_TICK:    needed = sizeof(TimerTickMsg);    break;
        case MSG_ROUND_EVENT:   needed = sizeof(RoundEventMsg);   break;
        case MSG_PLAYER_ELIM:   needed = sizeof(PlayerElimMsg);   break;
        case MSG_GAME_OVER:     needed = sizeof(GameOverMsg);     break;
        case MSG_ERROR:         needed = sizeof(ErrorMsg);        break;
        case MSG_WAIT:          needed = sizeof(WaitMsg);         break;
        case MSG_CHAR_LIST:     needed = sizeof(CharListMsg);     break;
        case MSG_STATUS_UPDATE: needed = sizeof(StatusUpdateMsg); break;
        default:
            fprintf(stderr, "Unknown opcode 0x%02x from server\n", opcode);
            return -1;
        }

        if ((size_t)rbuf_len < needed) break;

        switch (opcode) {
        case MSG_LOBBY_UPDATE:  on_lobby_update((const LobbyUpdateMsg *)rbuf); break;
        case MSG_JOIN_ACK:      on_join_ack((const JoinAckMsg *)rbuf);         break;
        case MSG_GAME_START:    on_game_start((const GameStartMsg *)rbuf);     break;
        case MSG_ROUND_START:   on_round_start((const RoundStartMsg *)rbuf);   break;
        case MSG_TIMER_TICK:    on_timer_tick((const TimerTickMsg *)rbuf);      break;
        case MSG_ROUND_EVENT:   on_round_event((const RoundEventMsg *)rbuf);   break;
        case MSG_PLAYER_ELIM:   on_player_elim((const PlayerElimMsg *)rbuf);   break;
        case MSG_GAME_OVER:     on_game_over((const GameOverMsg *)rbuf);       break;
        case MSG_ERROR:         on_error((const ErrorMsg *)rbuf);              break;
        case MSG_WAIT:          on_wait();                                     break;
        case MSG_CHAR_LIST:     on_char_list((const CharListMsg *)rbuf);       break;
        case MSG_STATUS_UPDATE: on_status_update((const StatusUpdateMsg *)rbuf); break;
        }

        int rem = rbuf_len - (int)needed;
        if (rem > 0) memmove(rbuf, rbuf + needed, (size_t)rem);
        rbuf_len = rem;
    }
    return 0;
}

/* =========================================================================
 * Prompt and send action
 * ========================================================================= */

static void prompt_action(int sockfd)
{
    (void)sockfd;
    if (my_id < 0 || waiting_for_result || in_resolution) return;

    printf("\nYour turn! Choose an action:\n");
    for (int i = 0; i < action_count; i++) {
        printf("  %d) %-20s -- %s",
               actions[i].action_id,
               actions[i].name,
               actions[i].desc);
        if (actions[i].requires_target == 1)
            printf("  [Usage: %d <target_id>]", actions[i].action_id);
        else if (actions[i].requires_target == 2)
            printf("  [Usage: %d <player_id>]", actions[i].action_id);
        printf("\n");
    }
    printf("Time remaining: --s\n> ");
    timer_line_displayed = 1;
    fflush(stdout);
}

static void handle_stdin_lobby(int sockfd)
{
    char line[128];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        printf("EOF on stdin.\n");
        close(sockfd);
        exit(0);
    }
    line[strcspn(line, "\n")] = '\0';

    if (strncmp(line, "pick ", 5) == 0 || strncmp(line, "pick\t", 5) == 0) {
        int cid = atoi(line + 5);
        CharSelectMsg m;
        m.msg_type   = MSG_CHAR_SELECT;
        m.char_id    = (uint8_t)cid;
        m.padding[0] = 0;
        m.padding[1] = 0;
        if (write_all(sockfd, &m, sizeof(m)) < 0) {
            perror("write");
            exit(1);
        }
        char_selected = 1;
        my_char_id = cid;
        return;
    }

    if (am_host && (strcmp(line, "start") == 0 || line[0] == '\0')) {
        StartGameMsg sgm;
        sgm.msg_type   = MSG_START_GAME;
        sgm.padding[0] = 0;
        sgm.padding[1] = 0;
        sgm.padding[2] = 0;
        if (write_all(sockfd, &sgm, sizeof(sgm)) < 0) {
            perror("write");
            exit(1);
        }
        return;
    }

    /* Allow just a number for pick shorthand */
    int cid = -1;
    if (sscanf(line, "%d", &cid) == 1 && cid >= 0 && cid < NUM_CHARACTERS) {
        CharSelectMsg m;
        m.msg_type   = MSG_CHAR_SELECT;
        m.char_id    = (uint8_t)cid;
        m.padding[0] = 0;
        m.padding[1] = 0;
        if (write_all(sockfd, &m, sizeof(m)) < 0) {
            perror("write");
            exit(1);
        }
        char_selected = 1;
        my_char_id = cid;
        return;
    }

    printf("Commands: 'pick <0-7>' to select character");
    if (am_host) printf(", 'start' to begin game");
    printf("\n");
    fflush(stdout);
}

static void handle_stdin_game(int sockfd)
{
    char line[128];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        printf("EOF on stdin, disconnecting.\n");
        close(sockfd);
        exit(0);
    }
    line[strcspn(line, "\n")] = '\0';

    if (my_id < 0) return;

    int aid = -1, tid = NO_TARGET;
    int parsed = sscanf(line, "%d %d", &aid, &tid);

    if (parsed < 1 || aid < 0 || aid >= action_count) {
        printf("Enter action number (0-%d), optionally followed by target player ID.\n",
               action_count - 1);
        prompt_action(sockfd);
        return;
    }

    if (actions[aid].requires_target >= 1 && parsed < 2) {
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

    /* Event loop */
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
        }

        /* Read from stdin */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (my_id >= 0 && !waiting_for_result && !game_over
                && !in_resolution) {
                handle_stdin_game(sockfd);
                need_prompt = 0;
            } else if (my_id < 0 && !game_over) {
                handle_stdin_lobby(sockfd);
            } else {
                /* Drain stdin */
                char tmp[128];
                if (fgets(tmp, sizeof(tmp), stdin) == NULL) break;
            }
        }

        if (need_prompt && !waiting_for_result && !in_resolution) {
            prompt_action(sockfd);
            need_prompt = 0;
        }
    }

    close(sockfd);
    return 0;
}
