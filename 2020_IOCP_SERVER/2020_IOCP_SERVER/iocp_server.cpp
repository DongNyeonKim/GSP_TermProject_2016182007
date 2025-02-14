#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <queue>
#include <sqlext.h>
#include <windows.h>
#include <stdio.h> 
#include "Default.h"
#include "Astar.h"

extern "C" {
#include "include/lua.h"
#include "include/lauxlib.h"
#include "include/lualib.h"
}


#include "protocol.h"
using namespace std;
using namespace chrono;

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "lua54.lib")
#pragma comment(lib, "odbc32.lib")

constexpr int MAX_BUFFER = 4096;

constexpr char OP_MODE_RECV = 0;
constexpr char OP_MODE_SEND = 1;
constexpr char OP_MODE_ACCEPT = 2;
constexpr char OP_RANDOM_MOVE = 3;
constexpr char OP_PLAYER_MOVE_NOTIFY = 4;
constexpr char OP_PLAYER_MOVE_1s = 5;
constexpr char OP_PLAYER_ATTACK_1s = 6;
constexpr char OP_NPC_RESPAWN = 7;
constexpr char OP_NPC_ATTACK_1s = 8;
constexpr char OP_PLAYER_HP_RECOVERY_5s = 9;

constexpr int  KEY_SERVER = 1000000;

struct OVER_EX {
    WSAOVERLAPPED wsa_over;
    char   op_mode;
    WSABUF   wsa_buf;
    unsigned char iocp_buf[MAX_BUFFER];
    int		object_id;
};

struct client_info {
    mutex c_lock;
    int id;
    char name[MAX_ID_LEN];
    short first_x, first_y;
    short x, y;
    short hp;
    short level;
    short exp;

    bool move_1s_time;
    bool attack_1s_time;
    bool hp_recov_5s_time;
    bool recovery = false;

    mutex lua_lock;
    lua_State* L;

    bool in_use;
    atomic_bool is_active;
    SOCKET   m_sock;
    OVER_EX   m_recv_over;
    unsigned char* m_packet_start;
    unsigned char* m_recv_start;

    mutex vl;
    unordered_set <int> view_list;

    int move_time;

    //NPC
    bool live = false;
    bool fixed;
    bool attack_type;
    bool attack = false;
};

mutex id_lock;

client_info g_clients[MAX_USER + NUM_NPC];

//4분할 섹터
vector <int> sec1;
vector <int> sec2;
vector <int> sec3;
vector <int> sec4;


HANDLE      h_iocp;

SOCKET g_lSocket;
OVER_EX g_accept_over;

struct event_type {
    int obj_id;
    system_clock::time_point wakeup_time;
    int event_id;
    int target_id;
    constexpr bool operator < (const event_type& _Left) const
    {
        return (wakeup_time > _Left.wakeup_time);
    }
};

priority_queue<event_type> timer_queue;

mutex timer_l;

void random_move_npc(int id);
void disconnect_client(int id);
bool find_db(int id);
void insert_db(int id, int x, int y, int hp, int level, int exp, int f_x, int f_y);
void update_db(int id, int x, int y, int hp, int level, int exp, int f_x, int f_y);
int API_get_x(lua_State* L);
int API_get_y(lua_State* L);
int API_SendMessage(lua_State* L);
bool is_npc(int p1);
void npc_die(int npc_id);
void wake_up_npc(int id);


void error_display(const char* msg, int err_no)
{
    WCHAR* lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, err_no,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&lpMsgBuf, 0, NULL);
    std::cout << msg;
    std::wcout << L"에러 " << lpMsgBuf << std::endl;
    while (true);
    LocalFree(lpMsgBuf);
}

//타이머 관련 함수
void add_timer(int obj_id, int ev_type, system_clock::time_point t, int target_id)
{
    timer_l.lock();
    event_type ev{ obj_id,t,ev_type,target_id };
    timer_queue.push(ev);
    timer_l.unlock();
}

void time_worker()
{
    while (true) {
        while (true) {
            if (false == timer_queue.empty())
            {
                event_type ev = timer_queue.top();
                if (ev.wakeup_time > system_clock::now())break;
                timer_l.lock();
                timer_queue.pop();
                timer_l.unlock();

                if (ev.event_id == OP_RANDOM_MOVE) {
                    OVER_EX* send_over = new OVER_EX();
                    send_over->op_mode = ev.event_id;
                    PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &send_over->wsa_over);
                    break;
                }
                else if (ev.event_id == OP_NPC_RESPAWN) {
                    OVER_EX* send_over = new OVER_EX();
                    send_over->op_mode = ev.event_id;
                    PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &send_over->wsa_over);
                    break;
                }
                else if (ev.event_id == OP_PLAYER_MOVE_1s) {
                    OVER_EX* send_over = new OVER_EX();
                    send_over->op_mode = ev.event_id;
                    PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &send_over->wsa_over);
                    break;
                }
                else if (ev.event_id == OP_PLAYER_ATTACK_1s) {
                    OVER_EX* send_over = new OVER_EX();
                    send_over->op_mode = ev.event_id;
                    PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &send_over->wsa_over);
                    break;
                }
                else if (ev.event_id == OP_NPC_ATTACK_1s) {
                    OVER_EX* send_over = new OVER_EX();
                    send_over->op_mode = ev.event_id;
                    send_over->object_id = ev.target_id;
                    PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &send_over->wsa_over);
                    break;
                }
                else if (ev.event_id == OP_PLAYER_HP_RECOVERY_5s) {
                    OVER_EX* send_over = new OVER_EX();
                    send_over->op_mode = ev.event_id;
                    PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &send_over->wsa_over);
                    break;
                }
            }
            else {
                break;
            }

        }
        this_thread::sleep_for(1ms);
    }
}



//거리 판단 함수
bool is_near(int p1, int p2)
{
    int dist = (g_clients[p1].x - g_clients[p2].x) * (g_clients[p1].x - g_clients[p2].x);
    dist += (g_clients[p1].y - g_clients[p2].y) * (g_clients[p1].y - g_clients[p2].y);

    return dist <= VIEW_LIMIT * VIEW_LIMIT;
}

bool is_near_dis(int p1, int p2, int dis)
{
    int dist = (g_clients[p1].x - g_clients[p2].x) * (g_clients[p1].x - g_clients[p2].x);
    dist += (g_clients[p1].y - g_clients[p2].y) * (g_clients[p1].y - g_clients[p2].y);

    return dist <= dis * dis;
}

bool is_in_moverange(short x, short y, short n_x, short n_y, int dis)
{
    int dist = (x - n_x) * (x - n_x);
    dist += (y - n_y) * (y - n_y);

    return dist <= dis * dis;
}

int get_moverange(short x, short y, short n_x, short n_y)
{
    int dist = (x - n_x) * (x - n_x);
    dist += (y - n_y) * (y - n_y);

    return dist;
}

bool is_in_normal_attack_range(int Player_id, int NPC_ID)
{
    if (g_clients[Player_id].x == g_clients[NPC_ID].x && g_clients[Player_id].y == g_clients[NPC_ID].y) {
        return true;
    }
    else if (g_clients[Player_id].x - 1 == g_clients[NPC_ID].x && g_clients[Player_id].y == g_clients[NPC_ID].y) {
        return true;
    }
    else if (g_clients[Player_id].x + 1 == g_clients[NPC_ID].x && g_clients[Player_id].y == g_clients[NPC_ID].y) {
        return true;
    }
    else if (g_clients[Player_id].y - 1 == g_clients[NPC_ID].y && g_clients[Player_id].x == g_clients[NPC_ID].x) {
        return true;
    }
    else if (g_clients[Player_id].y + 1 == g_clients[NPC_ID].y && g_clients[Player_id].x == g_clients[NPC_ID].x) {
        return true;
    }
    else
        return false;
}

bool is_in_range_attack_range(int Player_id, int NPC_ID)
{
    if (g_clients[Player_id].x == g_clients[NPC_ID].x && g_clients[Player_id].y == g_clients[NPC_ID].y) {
        return true;
    }
    else if (g_clients[Player_id].x - 1 == g_clients[NPC_ID].x && g_clients[Player_id].y == g_clients[NPC_ID].y) {
        return true;
    }
    else if (g_clients[Player_id].x + 1 == g_clients[NPC_ID].x && g_clients[Player_id].y == g_clients[NPC_ID].y) {
        return true;
    }
    else if (g_clients[Player_id].y - 1 == g_clients[NPC_ID].y && g_clients[Player_id].x == g_clients[NPC_ID].x) {
        return true;
    }
    else if (g_clients[Player_id].y + 1 == g_clients[NPC_ID].y && g_clients[Player_id].x == g_clients[NPC_ID].x) {
        return true;
    }
    else if (g_clients[Player_id].y -1 == g_clients[NPC_ID].y && g_clients[Player_id].x -1 == g_clients[NPC_ID].x) {
        return true;
    }
    else if (g_clients[Player_id].y - 1 == g_clients[NPC_ID].y && g_clients[Player_id].x + 1 == g_clients[NPC_ID].x) {
        return true;
    }
    else if (g_clients[Player_id].y + 1 == g_clients[NPC_ID].y && g_clients[Player_id].x - 1 == g_clients[NPC_ID].x) {
        return true;
    }
    else if (g_clients[Player_id].y + 1 == g_clients[NPC_ID].y && g_clients[Player_id].x + 1 == g_clients[NPC_ID].x) {
        return true;
    }
    else
        return false;
}

bool same_position(short x, short y, short x1, short y1) {
    if (x == x1 && y == y1)
        return true;
    else
        return false;
}


//패킷 전송 함수
void send_packet(int id, void* p)
{
    unsigned char* packet = reinterpret_cast<unsigned char*>(p);
    OVER_EX* send_over = new OVER_EX;
    memcpy(send_over->iocp_buf, packet, packet[0]);
    send_over->op_mode = OP_MODE_SEND;
    send_over->wsa_buf.buf = reinterpret_cast<CHAR*>(send_over->iocp_buf);
    send_over->wsa_buf.len = packet[0];
    ZeroMemory(&send_over->wsa_over, sizeof(send_over->wsa_over));
    g_clients[id].c_lock.lock();
    if (true == g_clients[id].in_use)
        WSASend(g_clients[id].m_sock, &send_over->wsa_buf, 1,
            NULL, 0, &send_over->wsa_over, NULL);
    g_clients[id].c_lock.unlock();
}

void send_chat_packet(int to_client, int id, char* mess)
{
    sc_packet_chat p;
    p.id = id;
    p.size = sizeof(p);
    p.type = SC_PACKET_CHAT;
    strcpy_s(p.message, mess);
    send_packet(to_client, &p);
}

void send_item_posi_packet(int to_client, int i)
{
    sc_packet_item_posi p;
    p.size = sizeof(p);
    p.type = SC_PACKET_ITEM_POSI;
    p.id = i;
    p.x = items[i].x;
    p.y = items[i].y;
    send_packet(to_client, &p);
}

void send_login_ok(int id)
{
    sc_packet_login_ok p;
    p.size = sizeof(p);
    p.type = SC_PACKET_LOGIN_OK;
    p.id = id;
    p.x = g_clients[id].x;
    p.y = g_clients[id].y;
    p.exp = g_clients[id].exp;
    p.hp = g_clients[id].hp;
    p.level = g_clients[id].level;
    send_packet(id, &p);
}

void send_login_fail_packet(int to_client, int id, char* mess)
{
    sc_packet_login_fail p;
    p.id = id;
    p.size = sizeof(p);
    p.type = SC_PACKET_LOGIN_FAIL;
    strcpy_s(p.message, mess);
    send_packet(to_client, &p);
}

void send_move_packet(int to_client, int id)
{
    sc_packet_move p;
    p.id = id;
    p.size = sizeof(p);
    p.type = SC_PACKET_MOVE;
    p.x = g_clients[id].x;
    p.y = g_clients[id].y;
    p.move_time = g_clients[id].move_time;
    send_packet(to_client, &p);
}

void send_change_state_packet(int player_id, int id, int npc)
{
    sc_packet_stat_chage p;
    p.size = sizeof(p);
    p.type = SC_PACKET_STAT_CHANGE;
    p.id = id;
    p.hp = g_clients[id].hp;
    p.exp = g_clients[id].exp;
    p.level = g_clients[id].level;
    strcpy_s(p.npc_name, g_clients[npc].name);

    send_packet(player_id, &p);
}

void send_enter_packet(int to_client, int new_id)
{
    sc_packet_enter p;
    p.id = new_id;
    p.size = sizeof(p);
    p.type = SC_PACKET_ENTER;
    p.x = g_clients[new_id].x;
    p.y = g_clients[new_id].y;
    p.hp = g_clients[new_id].hp;
    p.level = g_clients[new_id].level;
    g_clients[new_id].c_lock.lock();
    strcpy_s(p.name, g_clients[new_id].name);
    g_clients[new_id].c_lock.unlock();
    p.o_type = 0;
    send_packet(to_client, &p);
}

void send_leave_packet(int to_client, int new_id)
{
    sc_packet_leave p;
    p.id = new_id;
    p.size = sizeof(p);
    p.type = SC_PACKET_LEAVE;
    send_packet(to_client, &p);
}


//플레이어 관련 함수
//레벨 업데이트
void set_player_level(int id)
{
    if (0 <= g_clients[id].exp && g_clients[id].exp < 100)
        g_clients[id].level = 0;
    else if (100 <= g_clients[id].exp && g_clients[id].exp < 200)
        g_clients[id].level = 1;
    else if (200 <= g_clients[id].exp && g_clients[id].exp < 400)
        g_clients[id].level = 2;
    else if (400 <= g_clients[id].exp && g_clients[id].exp < 800)
        g_clients[id].level = 3;
    else if (800 <= g_clients[id].exp && g_clients[id].exp < 1200)
        g_clients[id].level = 4;
    else if (1200 <= g_clients[id].exp)
        g_clients[id].level = 5;
    else
        g_clients[id].level = 0;

}
//HP 회복
void recovery_player_hp(int player_id)
{
    if (true == g_clients[player_id].hp_recov_5s_time) return;
    if (false == g_clients[player_id].recovery) return;
    //회복 중이면 중복 호출으 방지하기 위해 회복중임을 표시
    g_clients[player_id].recovery = true;

    //10프로 증가
    g_clients[player_id].hp += g_clients[player_id].hp / 10;

    if (g_clients[player_id].hp > PLAYER_MAX_HP)
        g_clients[player_id].hp = PLAYER_MAX_HP;

    send_change_state_packet(player_id, player_id, 0);

    //체력이 MAX미만일 경우 재 호출
    if (g_clients[player_id].hp < PLAYER_MAX_HP) {
        g_clients[player_id].hp_recov_5s_time = true;
        add_timer(player_id, OP_PLAYER_HP_RECOVERY_5s, system_clock::now() + 5000ms, 0);
    }
    else
        g_clients[player_id].recovery = false;

}
//죽은 뒤 부활
void player_die_and_respawn(int id)
{
    g_clients[id].view_list.clear();

    g_clients[id].x = g_clients[id].first_x;
    g_clients[id].y = g_clients[id].first_y;
    g_clients[id].hp = PLAYER_MAX_HP;
    g_clients[id].exp = g_clients[id].exp / 2;
    set_player_level(id);

    send_login_ok(id);
    //주위에 존재하는 플레이어, NPC 알려줌
    if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
    {
        for (int i : sec1) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
    {
        for (int i : sec1) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
        for (int i : sec2) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
    {
        for (int i : sec2) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
    {
        for (int i : sec1) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
        for (int i : sec3) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
    {
        for (int i : sec1) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
        for (int i : sec2) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
        for (int i : sec3) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
        for (int i : sec4) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
    {
        for (int i : sec2) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
        for (int i : sec4) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
    {
        for (int i : sec3) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
    {
        for (int i : sec3) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
        for (int i : sec4) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
    {
        for (int i : sec4) {
            if (true == g_clients[i].in_use)
                if (id != i) {
                    if (false == is_near(i, id)) continue;
                    if (0 == g_clients[i].view_list.count(id)) {
                        g_clients[i].vl.lock();
                        g_clients[i].view_list.insert(id);
                        g_clients[i].vl.unlock();
                        send_enter_packet(i, id);
                    }
                    if (0 == g_clients[id].view_list.count(i)) {
                        g_clients[id].vl.lock();
                        g_clients[id].view_list.insert(i);
                        g_clients[id].vl.unlock();
                        send_enter_packet(id, i);
                    }
                }
            if (is_npc(i))
            {
                if (false == is_near(id, i)) continue;
                g_clients[id].view_list.insert(i);
                send_enter_packet(id, i);
                wake_up_npc(i);
            }
        }
    }
    else
        cout << "Sector Get Error" << endl;

}
//경험치 업데이트
void update_player_exp(int id, int npc) {
    short exp = 0;
    exp = g_clients[npc].level * 5;

    if (g_clients[npc].attack_type == true) {
        exp = exp * 2;
    }
    if (g_clients[npc].fixed == false)
    {
        exp = exp * 2;
    }

    g_clients[id].exp += exp;
}
//공격 처리
void process_attack(int id, int attack_type)
{
    if (true == g_clients[id].attack_1s_time) return;

    for (auto& npc : g_clients[id].view_list) {
        if (true == is_in_normal_attack_range(id, npc) && attack_type == AT_NORMAL) {
            //해당 NPC HP 감소
            g_clients[npc].hp -= PLAYER_ATTACK_DAMAGE;

            //플레이어에게 NPC 상태 변화 알림
            send_change_state_packet(id, npc, npc);

            //NPC 공격 알림
            if (g_clients[npc].attack == false) {
                g_clients[npc].attack_1s_time = true;
                add_timer(npc, OP_NPC_ATTACK_1s, system_clock::now() + 1000ms, id);
            }

            //NPC가 죽으면
            if (g_clients[npc].hp <= 0) {
                g_clients[npc].hp = 0;

                //NPC 별로 exp 수정 필요
                update_player_exp(id, npc);

                //EXP 업데이트 후 레벨 업데이트
                set_player_level(id);

                //플레이어의 뷰리스트에서 삭제하고 플레이어에게 leave 패킷 전송
                //NPC 죽음 처리
                npc_die(npc);
                //플레이어의 State 변화와 어떤 몬스터 죽였는지 알려줌
                send_change_state_packet(id, id, npc);
            }
            g_clients[npc].lua_lock.lock();
            lua_getglobal(g_clients[npc].L, "set_hp");
            lua_pushnumber(g_clients[npc].L, g_clients[npc].hp);
            lua_pcall(g_clients[npc].L, 1, 1, 0);
            g_clients[npc].lua_lock.unlock();
        }
        if (true == is_in_range_attack_range(id, npc) && attack_type == AT_RANGE) {
            //해당 NPC HP 감소
            g_clients[npc].hp -= PLAYER_ATTACK_DAMAGE;

            //플레이어에게 NPC 상태 변화 알림
            send_change_state_packet(id, npc, npc);

            //NPC 공격 알림
            if (g_clients[npc].attack == false) {
                g_clients[npc].attack_1s_time = true;
                add_timer(npc, OP_NPC_ATTACK_1s, system_clock::now() + 1000ms, id);
            }
            //NPC가 죽으면
            if (g_clients[npc].hp <= 0) {
                g_clients[npc].hp = 0;

                //NPC 별로 exp 수정 필요
                update_player_exp(id, npc);

                //EXP 업데이트 후 레벨 업데이트
                set_player_level(id);

                //플레이어의 뷰리스트에서 삭제하고 플레이어에게 leave 패킷 전송
                //NPC 죽음 처리
                npc_die(npc);
                //플레이어의 State 변화와 어떤 몬스터 죽였는지 알려줌
                send_change_state_packet(id, id, npc);
            }
            g_clients[npc].lua_lock.lock();
            lua_getglobal(g_clients[npc].L, "set_hp");
            lua_pushnumber(g_clients[npc].L, g_clients[npc].hp);
            lua_pcall(g_clients[npc].L, 1, 1, 0);
            g_clients[npc].lua_lock.unlock();
        }
    }

    g_clients[id].attack_1s_time = true;
    add_timer(id, OP_PLAYER_ATTACK_1s, system_clock::now() + 1000ms, 0);
}
//이동 처리
void process_move(int id, char dir)
{
    if (true == g_clients[id].move_1s_time) return;
    short y = g_clients[id].y;
    short x = g_clients[id].x;
    switch (dir) {
    case MV_UP: if (y > 0) y--; break;
    case MV_DOWN: if (y < (WORLD_HEIGHT - 1)) y++; break;
    case MV_LEFT: if (x > 0) x--; break;
    case MV_RIGHT: if (x < (WORLD_WIDTH - 1)) x++; break;
    default: cout << "Unknown Direction in CS_MOVE packet.\n";
        while (true);
    }

    //장애물과 충돌처리
    bool collision_obtacle = false;
    for (int i = 0; i < NUM_OBTACLE; i++)
    {
        if (ob_positions[i].x == x && ob_positions[i].y == y)
            collision_obtacle = true;
    }
    unordered_set <int> old_viewlist = g_clients[id].view_list;

    if (!collision_obtacle) {
        g_clients[id].x = x;
        g_clients[id].y = y;
    }

    //아이템과 충돌처리
    for (int i = 0; i < NUM_ITEM; i++)
    {
        if (items[i].x == g_clients[id].x && items[i].y == g_clients[id].y)
        {
            //플레이어 체력 업데이트
            g_clients[id].hp += 50;
            //상태 변화 알림
            send_change_state_packet(id, id, 0);
            //아이템 위치 변화
            items[i].x = rand() % (WORLD_WIDTH-50)+20;
            items[i].y = rand() % (WORLD_HEIGHT-50)+20;
            //아이템 위치 전송
            send_item_posi_packet(id, i);

            break;
        }
    }

    if (0 <= g_clients[id].x && g_clients[id].x <= 400 && 0 <= g_clients[id].y && g_clients[id].y <= 400) {
        if (std::find(sec1.begin(), sec1.end(), id) != sec1.end() == false) {
            sec1.push_back(id);
            sec2.erase(std::remove(sec2.begin(), sec2.end(), id), sec2.end());
            sec3.erase(std::remove(sec3.begin(), sec3.end(), id), sec3.end());
        }
    }
    else if (401 <= g_clients[id].x && g_clients[id].x <= 800 && 0 <= g_clients[id].y && g_clients[id].y <= 400) {
        if (std::find(sec2.begin(), sec2.end(), id) != sec2.end() == false) {
            sec2.push_back(id);
            sec1.erase(std::remove(sec1.begin(), sec1.end(), id), sec1.end());
            sec4.erase(std::remove(sec4.begin(), sec4.end(), id), sec4.end());
        }
    }
    else if (0 <= g_clients[id].x && g_clients[id].x <= 400 && 401 <= g_clients[id].y && g_clients[id].y <= 800) {
        if (std::find(sec3.begin(), sec3.end(), id) != sec3.end() == false) {
            sec3.push_back(id);
            sec1.erase(std::remove(sec1.begin(), sec1.end(), id), sec1.end());
            sec4.erase(std::remove(sec4.begin(), sec4.end(), id), sec4.end());
        }
    }
    else if (401 <= g_clients[id].x && g_clients[id].x <= 800 && 401 <= g_clients[id].y && g_clients[id].y <= 800) {
        if (std::find(sec4.begin(), sec4.end(), id) != sec4.end() == false) {
            sec4.push_back(id);
            sec2.erase(std::remove(sec2.begin(), sec2.end(), id), sec2.end());
            sec3.erase(std::remove(sec3.begin(), sec3.end(), id), sec3.end());
        }
    }
    else
        cout << "Sector Move Error" << endl;

    send_move_packet(id, id);

    unordered_set <int> new_viewlist;
    if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
    {
        for (int i : sec1) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
    {
        for (int i : sec1) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
        for (int i : sec2) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
    {
        for (int i : sec2) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
    {
        for (int i : sec1) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
        for (int i : sec3) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
    {
        for (int i : sec1) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
        for (int i : sec2) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
        for (int i : sec3) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
        for (int i : sec4) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
    {
        for (int i : sec2) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
        for (int i : sec4) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
    {
        for (int i : sec3) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
    {
        for (int i : sec3) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
        for (int i : sec4) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }
        }
    }
    else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
    {
        for (int i : sec4) {
            if (is_npc(i))
            {
                if (true == is_near(id, i)) {
                    if (g_clients[i].live) {
                        new_viewlist.insert(i);
                        wake_up_npc(i);
                    }
                }
                else
                    g_clients[i].is_active = false;
            }
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near(id, i)) {
                g_clients[id].vl.lock();
                new_viewlist.insert(i);
                g_clients[id].vl.unlock();
            }

        }
    }
    else
        cout << "Sector Get Error" << endl;


    for (int ob : new_viewlist) {
        if (0 == old_viewlist.count(ob)) {
            g_clients[id].view_list.insert(ob);
            send_enter_packet(id, ob);

            if (false == is_npc(ob)) {
                if (0 == g_clients[ob].view_list.count(id)) {
                    g_clients[ob].view_list.insert(id);
                    send_enter_packet(ob, id);
                }
                else {
                    send_move_packet(ob, id);
                }
            }
        }
        else {  // 이전에도 시야에 있었고, 이동후에도 시야에 있는 객체
            if (false == is_npc(ob)) {
                if (0 != g_clients[ob].view_list.count(id)) {
                    send_move_packet(ob, id);
                }
                else
                {
                    g_clients[ob].view_list.insert(id);
                    send_enter_packet(ob, id);
                }
            }
        }
    }

    for (int ob : old_viewlist) {
        if (0 == new_viewlist.count(ob)) {
            g_clients[id].view_list.erase(ob);
            send_leave_packet(id, ob);
            if (false == is_npc(ob)) {
                if (0 != g_clients[ob].view_list.count(id)) {
                    g_clients[ob].view_list.erase(id);
                    send_leave_packet(ob, id);
                }
            }
        }
    }

    if (false == is_npc(id)) {
        for (auto& npc : new_viewlist) {
            if (false == is_npc(npc)) continue;
            OVER_EX* ex_over = new OVER_EX;
            ex_over->object_id = id;
            ex_over->op_mode = OP_PLAYER_MOVE_NOTIFY;
            PostQueuedCompletionStatus(h_iocp, 1, npc, &ex_over->wsa_over);
        }
    }

    g_clients[id].move_1s_time = true;
    add_timer(id, OP_PLAYER_MOVE_1s, system_clock::now() + 1000ms, 0);

}
//클라이언트 연결
void add_new_client(SOCKET ns)
{
    int i;
    id_lock.lock();
    for (i = 0; i < MAX_USER; ++i)
        if (false == g_clients[i].in_use) break;
    id_lock.unlock();
    if (MAX_USER == i) {
        cout << "Max user limit exceeded.\n";
        closesocket(ns);
    }
    else {
        g_clients[i].c_lock.lock();
        g_clients[i].in_use = true;
        g_clients[i].m_sock = ns;
        g_clients[i].name[0] = 0;
        g_clients[i].id = -1;
        g_clients[i].c_lock.unlock();

        g_clients[i].m_packet_start = g_clients[i].m_recv_over.iocp_buf;
        g_clients[i].m_recv_over.op_mode = OP_MODE_RECV;
        g_clients[i].m_recv_over.wsa_buf.buf
            = reinterpret_cast<CHAR*>(g_clients[i].m_recv_over.iocp_buf);
        g_clients[i].m_recv_over.wsa_buf.len = sizeof(g_clients[i].m_recv_over.iocp_buf);
        ZeroMemory(&g_clients[i].m_recv_over.wsa_over, sizeof(g_clients[i].m_recv_over.wsa_over));
        g_clients[i].m_recv_start = g_clients[i].m_recv_over.iocp_buf;

        CreateIoCompletionPort(reinterpret_cast<HANDLE>(ns), h_iocp, i, 0);
        DWORD flags = 0;
        int ret;
        g_clients[i].c_lock.lock();
        if (true == g_clients[i].in_use) {
            ret = WSARecv(g_clients[i].m_sock, &g_clients[i].m_recv_over.wsa_buf, 1, NULL,
                &flags, &g_clients[i].m_recv_over.wsa_over, NULL);
        }
        g_clients[i].c_lock.unlock();
        if (SOCKET_ERROR == ret) {
            int err_no = WSAGetLastError();
            if (ERROR_IO_PENDING != err_no)
                error_display("WSARecv : ", err_no);
        }
    }
    SOCKET cSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    g_accept_over.op_mode = OP_MODE_ACCEPT;
    g_accept_over.wsa_buf.len = static_cast<ULONG> (cSocket);
    ZeroMemory(&g_accept_over.wsa_over, sizeof(&g_accept_over.wsa_over));
    AcceptEx(g_lSocket, cSocket, g_accept_over.iocp_buf, 0, 32, 32, NULL, &g_accept_over.wsa_over);
}
//연결 종료
void disconnect_client(int id)
{
    //데이터 베이스 업데이트
    update_db(g_clients[id].id, g_clients[id].x, g_clients[id].y, g_clients[id].hp, g_clients[id].level, g_clients[id].exp, g_clients[id].first_x, g_clients[id].first_y);
    for (int i = 0; i < MAX_USER; ++i) {
        if (true == g_clients[i].in_use)
            if (i != id) {
                if (0 != g_clients[i].view_list.count(id)) {
                    g_clients[i].vl.lock();
                    g_clients[i].view_list.erase(id);
                    g_clients[i].vl.unlock();
                    send_leave_packet(i, id);
                }
            }
    }
    sec1.erase(std::remove(sec1.begin(), sec1.end(), id), sec1.end());
    sec2.erase(std::remove(sec2.begin(), sec2.end(), id), sec2.end());
    sec3.erase(std::remove(sec3.begin(), sec3.end(), id), sec3.end());
    sec4.erase(std::remove(sec4.begin(), sec4.end(), id), sec4.end());
    g_clients[id].c_lock.lock();
    g_clients[id].in_use = false;
    g_clients[id].view_list.clear();
    closesocket(g_clients[id].m_sock);
    g_clients[id].m_sock = 0;
    g_clients[id].first_x = 0;
    g_clients[id].first_y = 0;
    g_clients[id].x = 0;
    g_clients[id].y = 0;
    g_clients[id].hp = 0;
    g_clients[id].level = 0;
    g_clients[id].exp = 0;
    g_clients[id].c_lock.unlock();
}


//NPC 관련 함수
//NPC 초기화
void initialize_NPC()
{
    cout << "Initializing NPC" << endl;
    for (int i = MAX_USER; i < MAX_USER + NUM_NPC; ++i)
    {
        g_clients[i].x = rand() % WORLD_WIDTH;
        g_clients[i].y = rand() % WORLD_HEIGHT;
        g_clients[i].first_x = g_clients[i].x;
        g_clients[i].first_y = g_clients[i].y;
        if (0 <= g_clients[i].x && g_clients[i].x <= 400 && 0 <= g_clients[i].y && g_clients[i].y <= 400)
            sec1.push_back(i);
        else if (401 <= g_clients[i].x && g_clients[i].x <= 800 && 0 <= g_clients[i].y && g_clients[i].y <= 400)
            sec2.push_back(i);
        else if (0 <= g_clients[i].x && g_clients[i].x <= 400 && 401 <= g_clients[i].y && g_clients[i].y <= 800)
            sec3.push_back(i);
        else if (401 <= g_clients[i].x && g_clients[i].x <= 800 && 401 <= g_clients[i].y && g_clients[i].y <= 800)
            sec4.push_back(i);
        else
            cout << "Sector Add Error" << endl;

        //NPC 고정, 이동여부 초기화
        if (i % 2 == 0)
        {
            g_clients[i].fixed = true;
        }
        else
            g_clients[i].fixed = false;

        //어그로 타입, 일반 타입 초기화
        if (i < MAX_USER + (NUM_NPC) / 2) {
            g_clients[i].attack_type = true;
        }
        else
            g_clients[i].attack_type = false;

        char npc_name[50];
        sprintf_s(npc_name, "N%d", i);
        strcpy_s(g_clients[i].name, npc_name);
        g_clients[i].is_active = false;
        g_clients[i].live = true;

        //레벨 랜덤 설정 후 레벨 별로 Hp 설정
        switch (rand() % 5)
        {
        case 0: {
            g_clients[i].hp = 50;
            g_clients[i].level = 1;
            break;
        }
        case 1: {
            g_clients[i].hp = 100;
            g_clients[i].level = 2;
            break;
        }
        case 2: {
            g_clients[i].hp = 150;
            g_clients[i].level = 3;
            break;
        }
        case 3: {
            g_clients[i].hp = 200;
            g_clients[i].level = 4;
            break;
        }
        case 4: {
            g_clients[i].hp = 250;
            g_clients[i].level = 5;
            break;
        }
        }

        lua_State* L = g_clients[i].L = luaL_newstate();
        luaL_openlibs(L);

        int error = luaL_loadfile(L, "monster.lua");
        error = lua_pcall(L, 0, 0, 0);

        lua_getglobal(L, "set_uid");
        lua_pushnumber(L, i);
        lua_pcall(L, 1, 1, 0);

        lua_getglobal(L, "set_x");
        lua_pushnumber(L, g_clients[i].x);
        lua_pcall(L, 1, 1, 0);

        lua_getglobal(L, "set_y");
        lua_pushnumber(L, g_clients[i].y);
        lua_pcall(L, 1, 1, 0);

        lua_getglobal(L, "set_level");
        lua_pushnumber(L, g_clients[i].level);
        lua_pcall(L, 1, 1, 0);

        lua_getglobal(L, "set_hp");
        lua_pushnumber(L, g_clients[i].hp);
        lua_pcall(L, 1, 1, 0);

        lua_getglobal(L, "set_move_type");
        lua_pushnumber(L, g_clients[i].fixed);
        lua_pcall(L, 1, 1, 0);

        lua_getglobal(L, "set_attack_type");
        lua_pushnumber(L, g_clients[i].attack_type);
        lua_pcall(L, 1, 1, 0);


        lua_register(L, "API_SendMessage", API_SendMessage);
        lua_register(L, "API_get_x", API_get_x);
        lua_register(L, "API_get_y", API_get_y);

    }
    cout << "NPC initialize finished." << endl;
}
//NPC 랜덤 이동
void random_move_npc(int id)
{
    if (g_clients[id].live == false) return;
    //고정 캐릭터 일 경우 move 안함
    if (g_clients[id].fixed == true) return;

    if (g_clients[id].attack == true) return;
    //플레이어 뷰리스트를 갱신해줘야 함 NPC가 들어갈 때와 나갈 떄
    unordered_set <int> old_viewlist;
    for (int i = 0; i < MAX_USER; ++i) {
        if (false == g_clients[i].in_use) continue;
        if (true == is_near(id, i)) old_viewlist.insert(i);
    }

    //LUA
    g_clients[id].lua_lock.lock();
    lua_getglobal(g_clients[id].L, "my_x");		
    int x = lua_tonumber(g_clients[id].L, -1); 
    lua_getglobal(g_clients[id].L, "my_y");
    int y = lua_tonumber(g_clients[id].L, -1);
    lua_pop(g_clients[id].L, 2);
    g_clients[id].lua_lock.unlock();


    //움직이는 NPC 범위 내 이동
    if (!is_in_moverange(x, y, g_clients[id].first_x, g_clients[id].first_y, 20))
    {
        while (true) {
            switch (rand() % 4)
            {
            case 0: if (x > 0) x--; break;
            case 1: if (x < (WORLD_WIDTH - 1)) x++; break;
            case 2: if (y > 0) y--; break;
            case 3: if (y < (WORLD_HEIGHT - 1)) y++; break;
            }
            if (is_in_moverange(x, y, g_clients[id].first_x, g_clients[id].first_y, 20)) break;
            x = g_clients[id].x;
            y = g_clients[id].y;
        }
    }
    else
    {
        switch (rand() % 4)
        {
        case 0: if (x > 0) x--; break;
        case 1: if (x < (WORLD_WIDTH - 1)) x++; break;
        case 2: if (y > 0) y--; break;
        case 3: if (y < (WORLD_HEIGHT - 1)) y++; break;
        }
    }

    if (g_clients[id].attack_type == true) {
        int chase_player_id = -1;
        for (int i = 0; i < MAX_USER; ++i) {
            if (id == i) continue;
            if (false == g_clients[i].in_use) continue;
            if (true == is_near_dis(id, i, 10)) {
                chase_player_id = i;
                //cout << "ID"<<i << endl;
                break;
            }
        }
        if (chase_player_id != -1) {
            if (g_clients[chase_player_id].in_use == true) {
                if (!same_position(g_clients[id].x, g_clients[id].y, g_clients[chase_player_id].x, g_clients[chase_player_id].y))
                {

                    Astar::Coordinate A(g_clients[id].x, g_clients[id].y);
                    Astar::Coordinate B(g_clients[chase_player_id].x, g_clients[chase_player_id].y);

                    Astar astar(A, B);

                    int x1, y1;
                    x1 = astar.GetPos(2).x;
                    y1 = astar.GetPos(2).y;
                    if (is_in_moverange(x1, y1, g_clients[id].first_x, g_clients[id].first_y, 20))
                    {
                        x = x1;
                        y = y1;
                    }

                    //공격 범위에 들어오면 공격
                    if (is_in_normal_attack_range(id, chase_player_id)) {
                        if (g_clients[id].attack == false) {
                            g_clients[id].attack_1s_time = true;
                            add_timer(id, OP_NPC_ATTACK_1s, system_clock::now() + 1000ms, chase_player_id);
                        }
                    }
                }
                else {
                    x = g_clients[id].x;
                    y = g_clients[id].y;
                    //어그로 몬스터가 플레이어 좌표와 같아지면 공격
                    if (g_clients[id].attack == false) {
                        g_clients[id].attack_1s_time = true;
                        add_timer(id, OP_NPC_ATTACK_1s, system_clock::now() + 1000ms, chase_player_id);
                    }
                }
            }
        }
    }

    //장애물 충돌처리
    bool collision_obtacle = false;
    for (int i = 0; i < NUM_OBTACLE; i++)
    {
        if (ob_positions[i].x == x && ob_positions[i].y == y)
            collision_obtacle = true;
    }

    if (!collision_obtacle) {
        g_clients[id].x = x;
        g_clients[id].y = y;


        //LUA
        g_clients[id].lua_lock.lock();
        lua_getglobal(g_clients[id].L, "set_x");
        lua_pushnumber(g_clients[id].L, g_clients[id].x);
        lua_pcall(g_clients[id].L, 1, 1, 0);
        lua_getglobal(g_clients[id].L, "set_y");
        lua_pushnumber(g_clients[id].L, g_clients[id].y);
        lua_pcall(g_clients[id].L, 1, 1, 0);
        g_clients[id].lua_lock.unlock();

    }

    if (0 <= g_clients[id].x && g_clients[id].x <= 400 && 0 <= g_clients[id].y && g_clients[id].y <= 400) {
        if (std::find(sec1.begin(), sec1.end(), id) != sec1.end() == false) {
            sec1.push_back(id);
            sec2.erase(std::remove(sec2.begin(), sec2.end(), id), sec2.end());
            sec3.erase(std::remove(sec3.begin(), sec3.end(), id), sec3.end());
        }
    }
    else if (401 <= g_clients[id].x && g_clients[id].x <= 800 && 0 <= g_clients[id].y && g_clients[id].y <= 400) {
        if (std::find(sec2.begin(), sec2.end(), id) != sec2.end() == false) {
            sec2.push_back(id);
            sec1.erase(std::remove(sec1.begin(), sec1.end(), id), sec1.end());
            sec4.erase(std::remove(sec4.begin(), sec4.end(), id), sec4.end());
        }
    }
    else if (0 <= g_clients[id].x && g_clients[id].x <= 400 && 401 <= g_clients[id].y && g_clients[id].y <= 800) {
        if (std::find(sec3.begin(), sec3.end(), id) != sec3.end() == false) {
            sec3.push_back(id);
            sec1.erase(std::remove(sec1.begin(), sec1.end(), id), sec1.end());
            sec4.erase(std::remove(sec4.begin(), sec4.end(), id), sec4.end());
        }
    }
    else if (401 <= g_clients[id].x && g_clients[id].x <= 800 && 401 <= g_clients[id].y && g_clients[id].y <= 800) {
        if (std::find(sec4.begin(), sec4.end(), id) != sec4.end() == false) {
            sec4.push_back(id);
            sec2.erase(std::remove(sec2.begin(), sec2.end(), id), sec2.end());
            sec3.erase(std::remove(sec3.begin(), sec3.end(), id), sec3.end());
        }
    }
    else
        cout << "Sector Move Error" << endl;

    unordered_set <int> new_viewlist;
    for (int i = 0; i < MAX_USER; ++i) {
        if (id == i) continue;
        if (false == g_clients[i].in_use) continue;
        if (true == is_near(id, i)) {
            new_viewlist.insert(i);
        }
        else
            g_clients[id].is_active = false;
    }

    for (auto pl : old_viewlist) {
        if (0 < new_viewlist.count(pl)) {
            //멀티 스레드이기 때문에 뷰리스트에 있는지 없는지 확실하지 않음
            if (0 < g_clients[pl].view_list.count(id))
                send_move_packet(pl, id);
            else
            {
                g_clients[pl].view_list.insert(id);
                send_enter_packet(pl, id);
            }
        }
        else
        {
            if (0 < g_clients[pl].view_list.count(id)) {
                g_clients[pl].view_list.erase(id);
                send_leave_packet(pl, id);
                g_clients[id].is_active = false;
            }
        }
    }

    for (auto pl : new_viewlist) {
        if (0 == g_clients[pl].view_list.count(pl)) {
            if (0 == g_clients[pl].view_list.count(id)) {
                g_clients[pl].view_list.insert(id);
                send_enter_packet(pl, id);
            }
            else
                send_move_packet(pl, id);
        }
    }

    if (true == new_viewlist.empty()) {
        g_clients[id].is_active = false;
    }
    else {
        add_timer(id, OP_RANDOM_MOVE, system_clock::now() + 1000ms, 0);
    }

    g_clients[id].lua_lock.lock();
    lua_getglobal(g_clients[id].L, "count_move");
    lua_pcall(g_clients[id].L, 0, 0, 0);
    g_clients[id].lua_lock.unlock();
}
//NPC 부활
void respawn_npc(int npc_id)
{
    g_clients[npc_id].x = g_clients[npc_id].first_x;
    g_clients[npc_id].y = g_clients[npc_id].first_y;
    if (g_clients[npc_id].level == 1)
        g_clients[npc_id].hp = 50;
    else if (g_clients[npc_id].level == 2)
        g_clients[npc_id].hp = 100;
    else if (g_clients[npc_id].level == 3)
        g_clients[npc_id].hp = 150;
    else if (g_clients[npc_id].level == 4)
        g_clients[npc_id].hp = 200;
    else if (g_clients[npc_id].level == 5)
        g_clients[npc_id].hp = 250;


    g_clients[npc_id].live = true;

    //섹터링
    if (0 <= g_clients[npc_id].x && g_clients[npc_id].x <= 400 && 0 <= g_clients[npc_id].y && g_clients[npc_id].y <= 400)
        sec1.push_back(npc_id);
    else if (401 <= g_clients[npc_id].x && g_clients[npc_id].x <= 800 && 0 <= g_clients[npc_id].y && g_clients[npc_id].y <= 400)
        sec2.push_back(npc_id);
    else if (0 <= g_clients[npc_id].x && g_clients[npc_id].x <= 400 && 401 <= g_clients[npc_id].y && g_clients[npc_id].y <= 800)
        sec3.push_back(npc_id);
    else if (401 <= g_clients[npc_id].x && g_clients[npc_id].x <= 800 && 401 <= g_clients[npc_id].y && g_clients[npc_id].y <= 800)
        sec4.push_back(npc_id);
    else
        cout << "Sector Add Error" << endl;

    for (int i = 0; i < MAX_USER; ++i) {
        if (false == g_clients[i].in_use) continue;
        if (true == is_near(npc_id, i)) {
            g_clients[i].vl.lock();
            g_clients[i].view_list.insert(npc_id);
            g_clients[i].vl.unlock();
            g_clients[npc_id].vl.lock();
            g_clients[npc_id].view_list.insert(i);
            g_clients[npc_id].vl.unlock();

            send_enter_packet(i, npc_id);
            //WakeUp을 하면서 is_active를 True로 활성화
            wake_up_npc(npc_id);
        }
    }

    g_clients[npc_id].lua_lock.lock();
    lua_getglobal(g_clients[npc_id].L, "set_x");
    lua_pushnumber(g_clients[npc_id].L, g_clients[npc_id].x);
    lua_pcall(g_clients[npc_id].L, 1, 1, 0);

    lua_getglobal(g_clients[npc_id].L, "set_y");
    lua_pushnumber(g_clients[npc_id].L, g_clients[npc_id].y);
    lua_pcall(g_clients[npc_id].L, 1, 1, 0);

    lua_getglobal(g_clients[npc_id].L, "set_level");
    lua_pushnumber(g_clients[npc_id].L, g_clients[npc_id].level);
    lua_pcall(g_clients[npc_id].L, 1, 1, 0);

    lua_getglobal(g_clients[npc_id].L, "set_hp");
    lua_pushnumber(g_clients[npc_id].L, g_clients[npc_id].hp);
    lua_pcall(g_clients[npc_id].L, 1, 1, 0);
    g_clients[npc_id].lua_lock.unlock();
}
//NPC 죽음
void npc_die(int npc_id)
{
    g_clients[npc_id].is_active = false;
    g_clients[npc_id].live = false;
    g_clients[npc_id].attack = false;
    //해당 NPC 섹터에서 삭제
    sec1.erase(std::remove(sec1.begin(), sec1.end(), npc_id), sec1.end());
    sec2.erase(std::remove(sec2.begin(), sec2.end(), npc_id), sec2.end());
    sec3.erase(std::remove(sec3.begin(), sec3.end(), npc_id), sec3.end());
    sec4.erase(std::remove(sec4.begin(), sec4.end(), npc_id), sec4.end());

    for (int i = 0; i < MAX_USER; ++i) {
        if (true == g_clients[i].in_use) {
            if (false == is_near(i, npc_id)) continue;
            //NPC가 죽을 경우 해당 NPC를 보고 있는 모든 플레이어 에게 leave 패킷을 전송
            send_leave_packet(i, npc_id);

        }

    }

    add_timer(npc_id, OP_NPC_RESPAWN, system_clock::now() + 30s, 0);
}
//NPC 공격
void npc_attack(int npc_id, int player_id)
{
    if (true == g_clients[npc_id].attack_1s_time) return;
    if (g_clients[npc_id].live == false) return;
    if (g_clients[npc_id].is_active == false) return;

    if (is_in_normal_attack_range(npc_id, player_id))
    {
        g_clients[npc_id].attack = true;
        g_clients[player_id].hp -= MONSTER_ATTACK_DAMAGE;

        send_change_state_packet(player_id, player_id, npc_id);

        //HP가 2000보다 적으면 피회복 시작
        if (g_clients[player_id].hp < PLAYER_MAX_HP)
        {
            if (g_clients[player_id].recovery == false) {
                g_clients[player_id].recovery = true;
                g_clients[player_id].hp_recov_5s_time = true;
                add_timer(player_id, OP_PLAYER_HP_RECOVERY_5s, system_clock::now() + 5000ms, 0);
            }
        }
        //플레이어가 죽으면 처리

        if (g_clients[player_id].hp <= 0) {
            player_die_and_respawn(player_id);
        }

    }

    if (is_in_normal_attack_range(npc_id, player_id)) {
        g_clients[npc_id].attack_1s_time = true;
        add_timer(npc_id, OP_NPC_ATTACK_1s, system_clock::now() + 1000ms, player_id);
    }
    else {
        g_clients[npc_id].attack = false;
        g_clients[npc_id].is_active = false;
        wake_up_npc(npc_id);
    }

}
//NPC 인지 판단
bool is_npc(int p1)
{
    return p1 >= MAX_USER;
}
//NPC 깨우기
void wake_up_npc(int id)
{
    bool b = false;
    if (true == g_clients[id].is_active.compare_exchange_strong(b, true))
    {
        add_timer(id, OP_RANDOM_MOVE, system_clock::now() + 1000ms, 0);
    }
}



//패킷 처리 함수 
void process_packet(int id)
{
    char p_type = g_clients[id].m_packet_start[1];
    switch (p_type) {
    case CS_LOGIN: {
        cs_packet_login* p = reinterpret_cast<cs_packet_login*>(g_clients[id].m_packet_start);
        g_clients[id].c_lock.lock();
        strcpy_s(g_clients[id].name, p->name);
        g_clients[id].c_lock.unlock();

        //이미 접속한 아이디 인지 확인
        bool find_use = false;
        for (int i = 0; i < MAX_USER; i++) {
            if (g_clients[i].id == p->id && g_clients[i].in_use==true) {
                find_use = true;
            }
        }
        if (find_use == true) {
            send_login_fail_packet(id, id, (char*)"LOGIN FAIL : Another player is already logged in.");
            disconnect_client(id);
        }

        g_clients[id].id = p->id;
        if (!find_db(id)) {
            //새로운 데이터 포기화
            g_clients[id].x = rand() % WORLD_WIDTH;
            g_clients[id].y = rand() % WORLD_HEIGHT;
            g_clients[id].hp = PLAYER_MAX_HP;
            g_clients[id].level = 0;
            g_clients[id].exp = 0;
            g_clients[id].first_x = g_clients[id].x;
            g_clients[id].first_y = g_clients[id].y;
            //섹터추가
            if (0 <= g_clients[id].x && g_clients[id].x <= 400 && 0 <= g_clients[id].y && g_clients[id].y <= 400)
                sec1.push_back(id);
            else if (401 <= g_clients[id].x && g_clients[id].x <= 800 && 0 <= g_clients[id].y && g_clients[id].y <= 400)
                sec2.push_back(id);
            else if (0 <= g_clients[id].x && g_clients[id].x <= 400 && 401 <= g_clients[id].y && g_clients[id].y <= 800)
                sec3.push_back(id);
            else if (401 <= g_clients[id].x && g_clients[id].x <= 800 && 401 <= g_clients[id].y && g_clients[id].y <= 800)
                sec4.push_back(id);
            else
                cout << "Sector Add Error" << endl;
            insert_db(g_clients[id].id, g_clients[id].x, g_clients[id].y, g_clients[id].hp, g_clients[id].level, g_clients[id].exp, g_clients[id].first_x, g_clients[id].first_y);
        }

        //로그인 오케이 패킷 전송
        send_login_ok(id);


        //주위에 존재하는 플레이어, NPC 알려줌
        if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
        {
            for (int i : sec1) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
        {
            for (int i : sec1) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
            for (int i : sec2) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 0 <= g_clients[id].y && g_clients[id].y <= 380)
        {
            for (int i : sec2) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
        {
            for (int i : sec1) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
            for (int i : sec3) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
        {
            for (int i : sec1) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
            for (int i : sec2) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
            for (int i : sec3) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
            for (int i : sec4) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 381 <= g_clients[id].y && g_clients[id].y <= 419)
        {
            for (int i : sec2) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
            for (int i : sec4) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (0 <= g_clients[id].x && g_clients[id].x <= 380 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
        {
            for (int i : sec3) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (381 <= g_clients[id].x && g_clients[id].x <= 419 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
        {
            for (int i : sec3) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
            for (int i : sec4) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else if (420 <= g_clients[id].x && g_clients[id].x <= 800 && 420 <= g_clients[id].y && g_clients[id].y <= 800)
        {
            for (int i : sec4) {
                if (true == g_clients[i].in_use)
                    if (id != i) {
                        if (false == is_near(i, id)) continue;
                        if (0 == g_clients[i].view_list.count(id)) {
                            g_clients[i].vl.lock();
                            g_clients[i].view_list.insert(id);
                            g_clients[i].vl.unlock();
                            send_enter_packet(i, id);
                        }
                        if (0 == g_clients[id].view_list.count(i)) {
                            g_clients[id].vl.lock();
                            g_clients[id].view_list.insert(i);
                            g_clients[id].vl.unlock();
                            send_enter_packet(id, i);
                        }
                    }
                if (is_npc(i))
                {
                    if (false == is_near(id, i)) continue;
                    g_clients[id].view_list.insert(i);
                    send_enter_packet(id, i);
                    wake_up_npc(i);
                }
            }
        }
        else
            cout << "Sector Get Error" << endl;

        break;
    }
    case CS_MOVE: {
        cs_packet_move* p = reinterpret_cast<cs_packet_move*>(g_clients[id].m_packet_start);
        g_clients[id].move_time = p->move_time;
        process_move(id, p->direction);
        break;
    }
    case CS_ATTACK: {
        cs_packet_attack* p = reinterpret_cast<cs_packet_attack*>(g_clients[id].m_packet_start);
        process_attack(id, AT_NORMAL);
        break;
    }
    case CS_RANGE_ATTACK: {
        cs_packet_range_attack* p = reinterpret_cast<cs_packet_range_attack*>(g_clients[id].m_packet_start);
        process_attack(id, AT_RANGE);
        break;
    }
    case CS_LOGOUT: {
        cs_packet_logout* p = reinterpret_cast<cs_packet_logout*>(g_clients[id].m_packet_start);
        disconnect_client(id);
        break;
    }
    case CS_CHAT: {
        cs_packet_chat* p = reinterpret_cast<cs_packet_chat*>(g_clients[id].m_packet_start);
        //상대 플레이어 에게
        for (auto pl : g_clients[id].view_list)
        {
            if (!is_npc(pl))
                send_chat_packet(pl, id, p->message);
        }
        //나에게
        send_chat_packet(id, id, p->message);
        break;
    }
    default: cout << "Unknown Packet type [" << p_type << "] from Client [" << id << "]\n";
        while (true);
    }
}

constexpr int MIN_BUFF_SIZE = 1024;

void process_recv(int id, DWORD iosize)
{
    unsigned char p_size = g_clients[id].m_packet_start[0];
    unsigned char* next_recv_ptr = g_clients[id].m_recv_start + iosize;
    while (p_size <= next_recv_ptr - g_clients[id].m_packet_start) {
        process_packet(id);
        g_clients[id].m_packet_start += p_size;
        if (g_clients[id].m_packet_start < next_recv_ptr)
            p_size = g_clients[id].m_packet_start[0];
        else break;
    }

    long long left_data = next_recv_ptr - g_clients[id].m_packet_start;

    if ((MAX_BUFFER - (next_recv_ptr - g_clients[id].m_recv_over.iocp_buf))
        < MIN_BUFF_SIZE) {
        memcpy(g_clients[id].m_recv_over.iocp_buf,
            g_clients[id].m_packet_start, left_data);
        g_clients[id].m_packet_start = g_clients[id].m_recv_over.iocp_buf;
        next_recv_ptr = g_clients[id].m_packet_start + left_data;
    }
    DWORD recv_flag = 0;
    g_clients[id].m_recv_start = next_recv_ptr;
    g_clients[id].m_recv_over.wsa_buf.buf = reinterpret_cast<CHAR*>(next_recv_ptr);
    g_clients[id].m_recv_over.wsa_buf.len = MAX_BUFFER -
        static_cast<int>(next_recv_ptr - g_clients[id].m_recv_over.iocp_buf);

    g_clients[id].c_lock.lock();
    if (true == g_clients[id].in_use) {
        WSARecv(g_clients[id].m_sock, &g_clients[id].m_recv_over.wsa_buf,
            1, NULL, &recv_flag, &g_clients[id].m_recv_over.wsa_over, NULL);
    }
    g_clients[id].c_lock.unlock();
}

//Worker Thread
void worker_thread()
{
    // 반복
    //   - 이 쓰레드를 IOCP thread pool에 등록  => GQCS
    //   - iocp가 처리를 맞긴 I/O완료 데이터를 꺼내기 => GQCS
    //   - 꺼낸 I/O완료 데이터를 처리
    while (true) {
        DWORD io_size;
        int key;
        ULONG_PTR iocp_key;
        WSAOVERLAPPED* lpover;
        int ret = GetQueuedCompletionStatus(h_iocp, &io_size, &iocp_key, &lpover, INFINITE);
        key = static_cast<int>(iocp_key);
        //cout << "Completion Detected" << endl;
        if (FALSE == ret) {
            //오류난 플레이어 disconnect
            disconnect_client(key);
            error_display("hGQCS Error : ", WSAGetLastError());

        }

        OVER_EX* over_ex = reinterpret_cast<OVER_EX*>(lpover);
        switch (over_ex->op_mode) {
        case OP_MODE_ACCEPT:
            add_new_client(static_cast<SOCKET>(over_ex->wsa_buf.len));
            break;
        case OP_MODE_RECV:
            if (0 == io_size)
                disconnect_client(key);
            else {
                //cout << "Packet from Client [" << key << "]" << endl;
                process_recv(key, io_size);
            }
            break;
        case OP_MODE_SEND:
            delete over_ex;
            break;
        case OP_RANDOM_MOVE:
        {
            random_move_npc(key);
            delete over_ex;
            break;
        }
        case OP_NPC_RESPAWN: {
            respawn_npc(key);
            delete over_ex;
            break;
        }
        case OP_PLAYER_MOVE_NOTIFY: {
            g_clients[key].lua_lock.lock();
            lua_getglobal(g_clients[key].L, "event_player_move");
            lua_pushnumber(g_clients[key].L, over_ex->object_id);
            lua_pcall(g_clients[key].L, 1, 1, 0);
            g_clients[key].lua_lock.unlock();
            delete over_ex;
            break;
        }
        case OP_PLAYER_MOVE_1s: {
            g_clients[key].move_1s_time = false;
            delete over_ex;
            break;
        }
        case OP_PLAYER_ATTACK_1s: {
            g_clients[key].attack_1s_time = false;
            delete over_ex;
            break;
        }
        case OP_PLAYER_HP_RECOVERY_5s: {
            g_clients[key].hp_recov_5s_time = false;
            recovery_player_hp(key);
            delete over_ex;
            break;
        }
        case OP_NPC_ATTACK_1s: {
            g_clients[key].attack_1s_time = false;
            npc_attack(key, over_ex->object_id);
            delete over_ex;
            break;
        }
        }
    }
}


//스크립트 연동 함수
int API_get_x(lua_State* L)
{
    int user_id = lua_tointeger(L, -1);
    lua_pop(L, 2);
    int x = g_clients[user_id].x;
    lua_pushnumber(L, x);
    return 1;
}

int API_get_y(lua_State* L)
{
    int user_id = lua_tointeger(L, -1);
    lua_pop(L, 2);
    int y = g_clients[user_id].y;
    lua_pushnumber(L, y);
    return 1;
}

int API_SendMessage(lua_State* L)
{
    int my_id = (int)lua_tointeger(L, -3);
    int user_id = (int)lua_tointeger(L, -2);
    char* mess = (char*)lua_tostring(L, -1);

    lua_pop(L, 3);

    send_chat_packet(user_id, my_id, mess);
    return 0;
}


//DataBase  관련 함수
void show_error(SQLHANDLE hHandle, SQLSMALLINT hType, RETCODE RetCode)
{
    SQLSMALLINT iRec = 0;
    SQLINTEGER iError;
    WCHAR wszMessage[1000];
    WCHAR wszState[SQL_SQLSTATE_SIZE + 1];
    if (RetCode == SQL_INVALID_HANDLE) {
        wcout << L"Invalid handle!\n";
        return;
    }
    while (SQLGetDiagRec(hType, hHandle, ++iRec, wszState, &iError, wszMessage,
        (SQLSMALLINT)(sizeof(wszMessage) / sizeof(WCHAR)), (SQLSMALLINT*)NULL) == SQL_SUCCESS) {
        // Hide data truncated..
        if (wcsncmp(wszState, L"01004", 5)) {
            wcout << L"[" << wszState << L"]" << wszMessage << "(" << iError << ")" << endl;
        }
    }
}

bool find_db(int id)
{
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt = 0;
    SQLRETURN retcode;
    //데이터를 읽는 변수
    SQLINTEGER ID, POS_X, POS_Y, HP, LEVEL, EXP, F_X, F_Y;
    SQLLEN cbID = 0, cbPOS_X = 0, cbPOS_Y = 0, cbHP = 0, cbLEVEL = 0, cbEXP = 0, cbF_X = 0, cbF_Y = 0;

    SQLWCHAR query[1024];
    wsprintf(query, L"EXECUTE find_data %d ", g_clients[id].id);
    bool no_data = true;
    std::wcout.imbue(std::locale("korean"));

    // Allocate environment handle  
    retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

    // Set the ODBC version environment attribute  
    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
        retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

        // Allocate connection handle  
        if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
            retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

            // Set login timeout to 5 seconds  
            if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

                // Connect to data source  
                retcode = SQLConnect(hdbc, (SQLWCHAR*)L"2020_fall", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

                // Allocate statement handle  
                if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                    retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

                    retcode = SQLExecDirect(hstmt, (SQLWCHAR*)query, SQL_NTS);
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                        // Bind columns 1, 2, and 3;
                        retcode = SQLBindCol(hstmt, 1, SQL_C_LONG, &ID, 100, &cbID);
                        retcode = SQLBindCol(hstmt, 2, SQL_C_LONG, &POS_X, 100, &cbPOS_X);
                        retcode = SQLBindCol(hstmt, 3, SQL_C_LONG, &POS_Y, 100, &cbPOS_Y);
                        retcode = SQLBindCol(hstmt, 4, SQL_C_LONG, &HP, 100, &cbHP);
                        retcode = SQLBindCol(hstmt, 5, SQL_C_LONG, &LEVEL, 100, &cbLEVEL);
                        retcode = SQLBindCol(hstmt, 6, SQL_C_LONG, &EXP, 100, &cbEXP);
                        retcode = SQLBindCol(hstmt, 7, SQL_C_LONG, &F_X, 100, &cbF_X);
                        retcode = SQLBindCol(hstmt, 8, SQL_C_LONG, &F_Y, 100, &cbF_Y);

                        // Fetch and print each row of data. On an error, display a message and exit.  
                        for (int i = 0; ; i++) {

                            retcode = SQLFetch(hstmt);
                            if (retcode == SQL_ERROR || retcode == SQL_SUCCESS_WITH_INFO)
                                show_error(hstmt, SQL_HANDLE_STMT, retcode);
                            if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO)
                            {
                                no_data = false;
                                g_clients[id].x = POS_X;
                                g_clients[id].y = POS_Y;
                                g_clients[id].hp = HP;
                                g_clients[id].level = LEVEL;
                                g_clients[id].exp = EXP;
                                g_clients[id].first_x = F_X;
                                g_clients[id].first_y = F_Y;
                            }
                            else
                                break;
                        }

                    }

                    // Process data  
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                        SQLCancel(hstmt);
                        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
                    }

                    SQLDisconnect(hdbc);
                }

                SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
            }
        }
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
    if (no_data) {
        //cout << "데이터 없음" << endl;
        return false;
    }
    else {
        //cout << "데이터 있음" << endl;
        return true;
    }
}

void insert_db(int id, int x, int y, int hp, int level, int exp, int f_x, int f_y) {
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt = 0;
    SQLRETURN retcode;
    //데이터를 읽는 변수
    SQLWCHAR query[1024];

    wsprintf(query, L"EXECUTE insert_data %d, %d, %d, %d, %d, %d, %d ,%d", id, x, y, hp, level, exp, f_x, f_y);
    std::wcout.imbue(std::locale("korean"));

    // Allocate environment handle  
    retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

    // Set the ODBC version environment attribute  
    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
        retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

        // Allocate connection handle  
        if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
            retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

            // Set login timeout to 5 seconds  
            if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

                // Connect to data source  
                retcode = SQLConnect(hdbc, (SQLWCHAR*)L"2020_fall", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

                // Allocate statement handle  
                if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                    retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

                    retcode = SQLExecDirect(hstmt, (SQLWCHAR*)query, SQL_NTS);
                    show_error(hstmt, SQL_HANDLE_STMT, retcode);
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                    }

                    // Process data  
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                        SQLCancel(hstmt);
                        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
                    }

                    SQLDisconnect(hdbc);
                }

                SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
            }
        }
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
}

void update_db(int id, int x, int y, int hp, int level, int exp, int f_x, int f_y) {
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt = 0;
    SQLRETURN retcode;
    //데이터를 읽는 변수
    SQLWCHAR query[1024];

    wsprintf(query, L"EXECUTE update_data %d, %d, %d, %d, %d, %d, %d, %d", x, y, hp, level, exp, f_x, f_y, id);
    std::wcout.imbue(std::locale("korean"));

    // Allocate environment handle  
    retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

    // Set the ODBC version environment attribute  
    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
        retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

        // Allocate connection handle  
        if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
            retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

            // Set login timeout to 5 seconds  
            if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

                // Connect to data source  
                retcode = SQLConnect(hdbc, (SQLWCHAR*)L"2020_fall", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

                // Allocate statement handle  
                if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                    retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

                    retcode = SQLExecDirect(hstmt, (SQLWCHAR*)query, SQL_NTS);
                    show_error(hstmt, SQL_HANDLE_STMT, retcode);
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                    }

                    // Process data  
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                        SQLCancel(hstmt);
                        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
                    }

                    SQLDisconnect(hdbc);
                }

                SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
            }
        }
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
}


int main()
{
    std::wcout.imbue(std::locale("korean"));

    for (auto& cl : g_clients)
        cl.in_use = false;

    WSADATA WSAData;
    WSAStartup(MAKEWORD(2, 0), &WSAData);
    h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
    g_lSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_lSocket), h_iocp, KEY_SERVER, 0);

    SOCKADDR_IN serverAddr;
    memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    ::bind(g_lSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(g_lSocket, 5);

    SOCKET cSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    g_accept_over.op_mode = OP_MODE_ACCEPT;
    g_accept_over.wsa_buf.len = static_cast<int>(cSocket);
    ZeroMemory(&g_accept_over.wsa_over, sizeof(&g_accept_over.wsa_over));
    AcceptEx(g_lSocket, cSocket, g_accept_over.iocp_buf, 0, 32, 32, NULL, &g_accept_over.wsa_over);

    //NPC 초기화
    initialize_NPC();

    thread timer_thread{ time_worker };
    vector <thread> worker_threads;
    for (int i = 0; i < 6; ++i)
        worker_threads.emplace_back(worker_thread);
    for (auto& th : worker_threads)
        th.join();
    timer_thread.join();

    closesocket(g_lSocket);
    WSACleanup();
}