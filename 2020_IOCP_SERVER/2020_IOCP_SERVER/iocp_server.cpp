#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <queue>
#include "Default.h"
#include "Astar.h"
//시야에서 사라지는 경우 activation을 끊어야함 cas? 더이상 큐에 입력받지 못하도록
//timer가 아니라 work thread 에서 실행되도록 행야함 post큐
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

constexpr int MAX_BUFFER = 4096;

constexpr char OP_MODE_RECV = 0;
constexpr char OP_MODE_SEND = 1;
constexpr char OP_MODE_ACCEPT = 2;
constexpr char OP_RANDOM_MOVE = 3;
constexpr char OP_PLAYER_MOVE_NOTIFY = 4;
constexpr char OP_PLAYER_MOVE_1s = 5;

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
    char name[MAX_ID_LEN];
    short first_x, first_y;
    short x, y;
    short hp;
    short level;
    short exp;

    bool move_1s_time;

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

    //NPC
    int move_time;
    bool fixed;
    bool attack_type;
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

bool CAS(volatile bool* addr, bool expected, bool new_val)
{
    return atomic_compare_exchange_strong(reinterpret_cast<volatile atomic_bool*>(addr), &expected, new_val);
}


void add_timer(int obj_id, int ev_type, system_clock::time_point t)
{
    timer_l.lock();
    event_type ev{ obj_id,t,ev_type,0 };
    timer_queue.push(ev);
    timer_l.unlock();
}

void random_move_npc(int id);

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

                    //random_move_npc(ev.obj_id);
                    //add_timer(ev.obj_id, OP_RANDOM_MOVE, system_clock::now() + 1s);
                    break;
                }
                else if (ev.event_id == OP_PLAYER_MOVE_1s) {
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

void wake_up_npc(int id)
{
    //if (false == g_clients[id].is_active) {
    //    //g_clients[id].is_active = true; //CAS로 구현해서 이중 활성화를 막아야 한다.
    //    if (CAS(&g_clients[id].is_active, false, true))
    //        add_timer(id, OP_RANDOM_MOVE, system_clock::now() + 1s);
    //}
    bool b = false;
    if (true == g_clients[id].is_active.compare_exchange_strong(b, true))
    {
        add_timer(id, OP_RANDOM_MOVE, system_clock::now() + 1s);
    }
}

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

bool is_npc(int p1)
{
    return p1 >= MAX_USER;
}

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

void send_login_ok(int id)
{
    sc_packet_login_ok p;
    p.exp = 0;
    p.hp = 100;
    p.id = id;
    p.level = 1;
    p.size = sizeof(p);
    p.type = SC_PACKET_LOGIN_OK;
    p.x = g_clients[id].x;
    p.y = g_clients[id].y;
    send_packet(id, &p);
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

void send_enter_packet(int to_client, int new_id)
{
    sc_packet_enter p;
    p.id = new_id;
    p.size = sizeof(p);
    p.type = SC_PACKET_ENTER;
    p.x = g_clients[new_id].x;
    p.y = g_clients[new_id].y;
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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
                    new_viewlist.insert(i);
                    wake_up_npc(i);
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

    //이동후 NPC 집어넣기
    //for (int i = MAX_USER; i < MAX_USER + NUM_NPC; ++i) {
    //   if (true == is_near(id, i)) {
    //      new_viewlist.insert(i);
    //      wake_up_npc(i);
    //   }
    //   else
    //      g_clients[id].is_active = false;
    //}

    

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
    add_timer(id, OP_PLAYER_MOVE_1s, system_clock::now() + 1ms);

}

void process_packet(int id)
{
    char p_type = g_clients[id].m_packet_start[1];
    switch (p_type) {
    case CS_LOGIN: {
        cs_packet_login* p = reinterpret_cast<cs_packet_login*>(g_clients[id].m_packet_start);
        g_clients[id].c_lock.lock();
        strcpy_s(g_clients[id].name, p->name);
        g_clients[id].c_lock.unlock();
        send_login_ok(id);
        //주위에 존재하는 플레이어들을 알려줌 현재는 User만 알려줌 NPC도 알려줘야함
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

        //for (int i = 0; i< MAX_USER; ++i)
        //   if (true == g_clients[i].in_use)
        //      if (id != i) {
        //         if (false == is_near(i, id)) continue;
        //         if (0 == g_clients[i].view_list.count(id)) {
        //            g_clients[i].view_list.insert(id);
        //            send_enter_packet(i, id);
        //         }
        //         if (0 == g_clients[id].view_list.count(i)) {
        //            g_clients[id].view_list.insert(i);
        //            send_enter_packet(id, i);
        //         }
        //      }

        //플레이어 주변에 있는 NPC들을 viewlist에 추가
        //for (int i = MAX_USER; i < MAX_USER + NUM_NPC; ++i)
        //{
        //   if (false == is_near(id, i)) continue;
        //   g_clients[id].view_list.insert(i);
        //   send_enter_packet(id, i);
        //   wake_up_npc(i);
        //}
        break;
    }
    case CS_MOVE: {
        cs_packet_move* p = reinterpret_cast<cs_packet_move*>(g_clients[id].m_packet_start);
        g_clients[id].move_time = p->move_time;
        process_move(id, p->direction);
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
        //cout << "New Client [" << i << "] Accepted" << endl;
        g_clients[i].c_lock.lock();
        g_clients[i].in_use = true;
        g_clients[i].m_sock = ns;
        g_clients[i].name[0] = 0;
        g_clients[i].c_lock.unlock();

        g_clients[i].m_packet_start = g_clients[i].m_recv_over.iocp_buf;
        g_clients[i].m_recv_over.op_mode = OP_MODE_RECV;
        g_clients[i].m_recv_over.wsa_buf.buf
            = reinterpret_cast<CHAR*>(g_clients[i].m_recv_over.iocp_buf);
        g_clients[i].m_recv_over.wsa_buf.len = sizeof(g_clients[i].m_recv_over.iocp_buf);
        ZeroMemory(&g_clients[i].m_recv_over.wsa_over, sizeof(g_clients[i].m_recv_over.wsa_over));
        g_clients[i].m_recv_start = g_clients[i].m_recv_over.iocp_buf;

        g_clients[i].x = rand() % WORLD_WIDTH;
        g_clients[i].y = rand() % WORLD_HEIGHT;
        //섹터추가
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

void disconnect_client(int id)
{
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
    g_clients[id].c_lock.unlock();
}

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
            //bool active = false;
            //for (int i = 0; i < MAX_USER; ++i)
            //{
            //   if(true==is_near(key,i))
            //      if (g_clients[i].in_use)
            //      {
            //          active = true;
            //         break;
            //      }
            //}
            //if (true == active) add_timer(key, OP_RANDOM_MOVE, system_clock::now() + 1s);
            //else g_clients[key].is_active = false;

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
        }
        }
    }
}

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

        if (i < (MAX_USER + NUM_NPC) / 2) {
            g_clients[i].attack_type = true;
        }
        else
            g_clients[i].attack_type = false;
        g_clients[i].attack_type = true;

        char npc_name[50];
        sprintf_s(npc_name, "N%d", i);
        strcpy_s(g_clients[i].name, npc_name);
        g_clients[i].is_active = false;

        lua_State* L = g_clients[i].L = luaL_newstate();
        luaL_openlibs(L);

        int error = luaL_loadfile(L, "monster.lua");
        error = lua_pcall(L, 0, 0, 0);

        lua_getglobal(L, "set_uid");
        lua_pushnumber(L, i);
        lua_pcall(L, 1, 1, 0);

        lua_register(L, "API_SendMessage", API_SendMessage);
        lua_register(L, "API_get_x", API_get_x);
        lua_register(L, "API_get_y", API_get_y);

    }
    cout << "NPC initialize finished." << endl;
}

//void chase_player(int id, short &x, short &y)
//{
//    Astar::Coordinate A(0, 0);
//    Astar::Coordinate B(0, 4);
//
//    Astar astar(A, B);
//
//    x = astar.GetPos(2).x;
//    y = astar.GetPos(2).y;
//}

bool same_position(short x, short y, short x1, short y1) {
    if (x == x1 && y == y1)
        return true;
    else
        return false;
}

void random_move_npc(int id)
{
    if (g_clients[id].fixed == true) return;
    //플레이어 뷰리스트를 갱신해줘야 함 NPC가 들어갈 때와 나갈 떄
    unordered_set <int> old_viewlist;
    for (int i = 0; i < MAX_USER; ++i) {
        if (false == g_clients[i].in_use) continue;
        if (true == is_near(id, i)) old_viewlist.insert(i);
    }

    int  x = g_clients[id].x;
    int  y = g_clients[id].y;



    //움직이는 NPC 범위 내 이동
    if (!is_in_moverange(x, y, g_clients[id].first_x, g_clients[id].first_y, 5))
    {
        while (true) {
            switch (rand() % 4)
            {
                case 0: if (x > 0) x--; break;
                case 1: if (x < (WORLD_WIDTH - 1)) x++; break;
                case 2: if (y > 0) y--; break;
                case 3: if (y < (WORLD_HEIGHT - 1)) y++; break;
            }
            if (is_in_moverange(x, y, g_clients[id].first_x, g_clients[id].first_y, 5)) break;
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
            if (true == is_near_dis(id, i, 3)) {
                chase_player_id = i;
                //cout << "ID"<<i << endl;
                break;
            }
        }
        if (chase_player_id != -1) {
            if (g_clients[chase_player_id].in_use == true) {
                if (!same_position(g_clients[id].x, g_clients[id].y, g_clients[chase_player_id].x, g_clients[chase_player_id].y))
                {
                    //cout << g_clients[id].x << "    " << g_clients[id].y << endl;
                    //cout << g_clients[chase_player_id].x << "   " << g_clients[chase_player_id].y << endl;
                    Astar::Coordinate A(g_clients[id].x, g_clients[id].y);
                    Astar::Coordinate B(g_clients[chase_player_id].x, g_clients[chase_player_id].y);

                    Astar astar(A, B);
                    //cout << astar.GetPos(2).x << "   " << astar.GetPos(2).y << endl;
                    int x1, y1;
                    x1 = astar.GetPos(2).x;
                    y1 = astar.GetPos(2).y;
                    if (is_in_moverange(x1, y1, g_clients[id].first_x, g_clients[id].first_y, 5))
                    {
                        x = x1;
                        y = y1;
                    }
                    //cout << "x: " << x << "y: " << y << endl;
                }
                else {
                    //cout << "same posi" << endl;
                    x = g_clients[id].x;
                    y = g_clients[id].y;
                }
            }
        }
    }



    //g_clients[id].x = x;
    //g_clients[id].y = y;

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
        add_timer(id, OP_RANDOM_MOVE, system_clock::now() + 1s);
    }

    //for (auto pc : new_viewlist) {
    //    OVER_EX* over_ex = new OVER_EX;
    //    over_ex->object_id = pc;
    //    over_ex->op_mode = OP_PLAYER_MOVE_NOTIFY;
    //    PostQueuedCompletionStatus(h_iocp, 1, id, &over_ex->wsa_over);
    //}

    g_clients[id].lua_lock.lock();
    lua_getglobal(g_clients[id].L, "count_move");
    lua_pcall(g_clients[id].L, 0, 0, 0);
    g_clients[id].lua_lock.unlock();
}

//void npc_ai_thread()
//{
//    while (true) {
//        auto start_time = system_clock::now();
//        for (int i = MAX_USER; i < MAX_USER + NUM_NPC; ++i)
//            random_move_npc(i);
//        auto end_time = system_clock::now();
//        auto exec_time = end_time - start_time;
//        cout << "AI exec time = " << duration_cast<seconds>(exec_time).count() << "s\n";
//        this_thread::sleep_for(1s - (end_time - start_time));
//    }
//}

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

    //thread ai_thread{ npc_ai_thread };
    thread timer_thread{ time_worker };
    vector <thread> worker_threads;
    for (int i = 0; i < 6; ++i)
        worker_threads.emplace_back(worker_thread);
    for (auto& th : worker_threads)
        th.join();
    //ai_thread.join();
    timer_thread.join();

    closesocket(g_lSocket);
    WSACleanup();
}