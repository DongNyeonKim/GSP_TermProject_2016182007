#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <windows.h>
#include <iostream>
#include <unordered_map>
#include <chrono>
#include <queue>
#include <string>
using namespace std;
using namespace chrono;

#include "..\..\2020_IOCP_SERVER\2020_IOCP_SERVER\protocol.h"

sf::TcpSocket g_socket;

constexpr auto SCREEN_WIDTH = 19;
constexpr auto SCREEN_HEIGHT = 19;

constexpr auto TILE_WIDTH = 65;
constexpr auto WINDOW_WIDTH = TILE_WIDTH * SCREEN_WIDTH / 2 + 10;   // size of window
constexpr auto WINDOW_HEIGHT = TILE_WIDTH * SCREEN_WIDTH / 2 + 10;
constexpr auto BUF_SIZE = 200;

// 추후 확장용.
int NPC_ID_START = 10000;

int g_left_x;
int g_top_y;
int g_myid;

sf::RenderWindow* g_window;
sf::Font g_font;


queue<string> chatqueue;

class OBJECT {
private:
	bool m_showing;
	sf::Sprite m_sprite;

	char m_mess[MAX_STR_LEN];
	high_resolution_clock::time_point m_time_out;
	sf::Text m_text;
	sf::Text m_name;
	sf::Text m_level;
	sf::Text m_hp;

public:
	int m_x, m_y;
	char name[MAX_ID_LEN];
	short hp=0;
	short level=0;
	short exp=0;
	OBJECT(sf::Texture& t, int x, int y, int x2, int y2) {
		m_showing = false;
		m_sprite.setTexture(t);
		m_sprite.setTextureRect(sf::IntRect(x, y, x2, y2));
		m_time_out = high_resolution_clock::now();
	}
	OBJECT() {
		m_showing = false;
		m_time_out = high_resolution_clock::now();
	}
	void show()
	{
		m_showing = true;
	}
	void hide()
	{
		m_showing = false;
	}

	void a_move(int x, int y) {
		m_sprite.setPosition((float)x, (float)y);
	}

	void a_draw() {
		g_window->draw(m_sprite);
	}

	void move(int x, int y) {
		m_x = x;
		m_y = y;
	}
	void draw() {
		if (false == m_showing) return;
		float rx = (m_x - g_left_x) * 65.0f + 8;
		float ry = (m_y - g_top_y) * 65.0f + 8;
		m_sprite.setPosition(rx, ry);
		g_window->draw(m_sprite);
		m_name.setPosition(rx - 10, ry - 10);
		g_window->draw(m_name);
		m_level.setPosition(rx - 10, ry - 30);
		g_window->draw(m_level);
		m_hp.setPosition(rx - 10, ry - 50);
		g_window->draw(m_hp);

		if (high_resolution_clock::now() < m_time_out) {
			m_text.setPosition(rx - 10, ry - 50);
			m_text.setCharacterSize(50);
			g_window->draw(m_text);
		}
	}
	void set_name(char str[]) {
		m_name.setFont(g_font);
		m_name.setString(str);
		m_name.setFillColor(sf::Color(255, 255, 0));
		m_name.setStyle(sf::Text::Bold);
	}
	void set_level(int level) {
		m_level.setFont(g_font);
		char hp_buf[100];
		sprintf_s(hp_buf, "LEVEL:%d", level);
		m_level.setString(hp_buf);
		m_level.setFillColor(sf::Color(255, 0, 255));
		m_level.setStyle(sf::Text::Bold);
	}
	void set_hp(int hp) {
		m_hp.setFont(g_font);
		char hp_buf[100];
		sprintf_s(hp_buf, "HP:%d", hp);
		m_hp.setString(hp_buf);
		m_hp.setFillColor(sf::Color(255, 0, 0));
		m_hp.setStyle(sf::Text::Bold);
	}
	void add_chat(char chat[]) {
		m_text.setFont(g_font);
		m_text.setString(chat);
		m_time_out = high_resolution_clock::now() + 1s;
	}
};

OBJECT avatar;
unordered_map <int, OBJECT> npcs;

OBJECT white_tile;
OBJECT black_tile;
//장애물
OBJECT obtacle_tile;

sf::Texture* board;
sf::Texture* pieces;
//장애물
sf::Texture* obtacle;

bool is_npc(int p1)
{
	return p1 >= MAX_USER;
}

void client_initialize()
{
	board = new sf::Texture;
	pieces = new sf::Texture;
	obtacle = new sf::Texture;
	if (false == g_font.loadFromFile("cour.ttf")) {
		cout << "Font Loading Error!\n";
		while (true);
	}
	board->loadFromFile("chessmap.bmp");
	pieces->loadFromFile("chess2.png");
	obtacle->loadFromFile("obtimage.bmp");
	white_tile = OBJECT{ *board, 5, 5, TILE_WIDTH, TILE_WIDTH };
	black_tile = OBJECT{ *board, 69, 5, TILE_WIDTH, TILE_WIDTH };
	obtacle_tile = OBJECT{ *obtacle, 5, 5, TILE_WIDTH, TILE_WIDTH };
	avatar = OBJECT{ *pieces, 128, 0, 64, 64 };
	avatar.move(4, 4);
}

void client_finish()
{
	delete board;
	delete pieces;
	delete obtacle;
}

void ProcessPacket(char* ptr)
{
	static bool first_time = true;
	switch (ptr[1])
	{
	case SC_PACKET_LOGIN_OK:
	{
		sc_packet_login_ok* my_packet = reinterpret_cast<sc_packet_login_ok*>(ptr);
		g_myid = my_packet->id;
		avatar.move(my_packet->x, my_packet->y);
		g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
		g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
		avatar.show();
	}
	break;

	case SC_PACKET_ENTER:
	{
		sc_packet_enter* my_packet = reinterpret_cast<sc_packet_enter*>(ptr);
		int id = my_packet->id;

		if (id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
			g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
			avatar.show();
		}
		else {
			if (id < NPC_ID_START)
				npcs[id] = OBJECT{ *pieces, 64, 0, 64, 64 };
			else
				npcs[id] = OBJECT{ *pieces, 0, 0, 64, 64 };

			strcpy_s(npcs[id].name, my_packet->name);
			npcs[id].set_name(my_packet->name);
			npcs[id].set_hp(my_packet->hp);
			npcs[id].set_level(my_packet->level);
			npcs[id].move(my_packet->x, my_packet->y);
			npcs[id].show();
		}
	}
	break;
	case SC_PACKET_MOVE:
	{
		sc_packet_move* my_packet = reinterpret_cast<sc_packet_move*>(ptr);
		int other_id = my_packet->id;
		if (other_id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
			g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
		}
		else {
			if (0 != npcs.count(other_id))
				npcs[other_id].move(my_packet->x, my_packet->y);
		}
	}
	break;

	case SC_PACKET_LEAVE:
	{
		sc_packet_leave* my_packet = reinterpret_cast<sc_packet_leave*>(ptr);
		int other_id = my_packet->id;
		if (other_id == g_myid) {
			avatar.hide();
		}
		else {
			if (0 != npcs.count(other_id))
				npcs[other_id].hide();
		}
	}
	break;
	case SC_PACKET_CHAT:
	{
		sc_packet_chat* my_packet = reinterpret_cast<sc_packet_chat*>(ptr);

		npcs[my_packet->id].add_chat(my_packet->message);

	}
	break;
	case SC_PACKET_STAT_CHANGE:
	{
		sc_packet_stat_chage* my_packet = reinterpret_cast<sc_packet_stat_chage*>(ptr);

		if (my_packet->id == g_myid)
		{
			avatar.hp = my_packet->hp;
			avatar.level = my_packet->level;
			

			string temp = "Defeated The Monster ";
			temp += my_packet->npc_name;
			temp += " AND Gained ";
			temp += to_string((my_packet->exp)- (avatar.exp));
			temp += "Exp.";

			avatar.exp = my_packet->exp;

			chatqueue.push(temp);
		}
		else if (is_npc(my_packet->id)) {
			npcs[my_packet->id].set_hp(my_packet->hp);
			string temp = "Warrior Attack Monster ";
			temp += my_packet->npc_name;
			temp += " Inclicting ";
			temp += to_string(PLAYER_ATTACK_DAMAGE);
			temp += "Damage.";

			chatqueue.push(temp);

		}


		//큐가 5개 이상일 경우 POP
		if (chatqueue.size() > 5)
			chatqueue.pop();
	}
	break;
	default:
		printf("Unknown PACKET type [%d]\n", ptr[1]);
	}
}

void process_data(char* net_buf, size_t io_byte)
{
	char* ptr = net_buf;
	static size_t in_packet_size = 0;
	static size_t saved_packet_size = 0;
	static char packet_buffer[BUF_SIZE];

	while (0 != io_byte) {
		if (0 == in_packet_size) in_packet_size = ptr[0];
		if (io_byte + saved_packet_size >= in_packet_size) {
			memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
			ProcessPacket(packet_buffer);
			ptr += in_packet_size - saved_packet_size;
			io_byte -= in_packet_size - saved_packet_size;
			in_packet_size = 0;
			saved_packet_size = 0;
		}
		else {
			memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
			saved_packet_size += io_byte;
			io_byte = 0;
		}
	}
}

void showChat()
{
	queue<string> temp;


	for (int i = 0; i < 5; i++)
	{
		if (!chatqueue.empty()) {

			sf::Text Chat;
			Chat.setFont(g_font);
			char hp_buf[100];

			strcpy_s(hp_buf, chatqueue.front().c_str());
			Chat.setString(hp_buf);
			Chat.setPosition(10, 950 + i*50);
			Chat.setCharacterSize(40);
			if(i%2==0)
				Chat.setFillColor(sf::Color::White);
			else
				Chat.setFillColor(sf::Color::Magenta);
			Chat.setStyle(sf::Text::Bold);
			g_window->draw(Chat);

			string cd;
			cd = chatqueue.front();
			chatqueue.pop();
			temp.push(cd);
		}
		else break;
	}
	chatqueue = temp;
}

void client_main()
{
	char net_buf[BUF_SIZE];
	size_t	received;

	auto recv_result = g_socket.receive(net_buf, BUF_SIZE, received);
	if (recv_result == sf::Socket::Error)
	{
		wcout << L"Recv 에러!";
		while (true);
	}

	if (recv_result == sf::Socket::Disconnected)
	{
		wcout << L"서버 접속 종료.";
		g_window->close();
	}

	if (recv_result != sf::Socket::NotReady)
		if (received > 0) process_data(net_buf, received);

	for (int i = 0; i < SCREEN_WIDTH; ++i) {
		int tile_x = i + g_left_x;
		if (tile_x >= WORLD_WIDTH) break;
		if (tile_x < 0) continue;
		for (int j = 0; j < SCREEN_HEIGHT; ++j)
		{
			int tile_y = j + g_top_y;
			if (tile_y >= WORLD_HEIGHT) break;
			if (tile_y < 0) continue;
			if (((tile_x / 3 + tile_y / 3) % 2) == 0) {
				white_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
				white_tile.a_draw();
			}
			else
			{
				black_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
				black_tile.a_draw();

			}
		}

	}
	//장애물 출력
	for (int i = 0; i < NUM_OBTACLE; ++i) {
		obtacle_tile.a_move(TILE_WIDTH * (ob_positions[i].x- g_left_x) + 7, TILE_WIDTH * (ob_positions[i].y- g_top_y) + 7);
		obtacle_tile.a_draw();
	}


	avatar.draw();
	//	for (auto &pl : players) pl.draw();
	for (auto& npc : npcs) npc.second.draw();

	//플레이어 위치 표시
	sf::Text text;
	text.setFont(g_font);
	char buf[100];
	sprintf_s(buf, "(%d, %d)", avatar.m_x, avatar.m_y);
	text.setString(buf);
	text.setCharacterSize(40);
	text.setStyle(sf::Text::Bold);
	g_window->draw(text);

	//플레이어 HP 표시
	sf::Text player_hp;
	player_hp.setFont(g_font);
	char hp_buf[100];
	sprintf_s(hp_buf, "HP: %d", avatar.hp);
	player_hp.setString(hp_buf);
	player_hp.setPosition(10, 50);
	player_hp.setCharacterSize(50);
	player_hp.setFillColor(sf::Color::Red);
	player_hp.setStyle(sf::Text::Bold);
	g_window->draw(player_hp);

	//플레이어 레벨 표시
	sf::Text player_level;
	player_level.setFont(g_font);
	char level_buf[100];
	sprintf_s(level_buf, "Level: %d", avatar.level);
	player_level.setString(level_buf);
	player_level.setPosition(10,100);
	player_level.setCharacterSize(50);
	player_level.setFillColor(sf::Color::Yellow);
	player_level.setStyle(sf::Text::Bold);
	g_window->draw(player_level);

	//플레이어 Exp 표시
	sf::Text player_exp;
	player_exp.setFont(g_font);
	char exp_buf[100];
	sprintf_s(exp_buf, "Exp: %d", avatar.exp);
	player_exp.setString(exp_buf);
	player_exp.setPosition(10, 150);
	player_exp.setCharacterSize(50);
	player_exp.setFillColor(sf::Color::Magenta);
	player_exp.setStyle(sf::Text::Bold);
	g_window->draw(player_exp);

	//채팅표시
	showChat();
}

void send_packet(void* packet)
{
	char* p = reinterpret_cast<char*>(packet);
	size_t sent;
	sf::Socket::Status st = g_socket.send(p, p[0], sent);
	int a = 3;
}

void send_move_packet(unsigned char dir)
{
	cs_packet_move m_packet;
	m_packet.type = CS_MOVE;
	m_packet.size = sizeof(m_packet);
	m_packet.direction = dir;
	send_packet(&m_packet);
}

void send_attack_packet()
{
	cs_packet_attack m_packet;
	m_packet.type = CS_ATTACK;
	m_packet.size = sizeof(m_packet);
	send_packet(&m_packet);
}

void send_logout_packet()
{
	cs_packet_logout m_packet;
	m_packet.type = CS_LOGOUT;
	m_packet.size = sizeof(m_packet);
	send_packet(&m_packet);
}


int main()
{
	wcout.imbue(locale("korean"));
	sf::Socket::Status status = g_socket.connect("127.0.0.1", SERVER_PORT);
	g_socket.setBlocking(false);

	if (status != sf::Socket::Done) {
		wcout << L"서버와 연결할 수 없습니다.\n";
		while (true);
	}

	client_initialize();

	cs_packet_login l_packet;
	l_packet.size = sizeof(l_packet);
	l_packet.type = CS_LOGIN;
	int t_id = GetCurrentProcessId();
	sprintf_s(l_packet.name, "P%03d", t_id % 1000);
	strcpy_s(avatar.name, l_packet.name);
	avatar.set_name(l_packet.name);
	send_packet(&l_packet);

	sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2D CLIENT");
	g_window = &window;

	sf::View view = g_window->getView();
	view.zoom(2.0f);
	view.move(SCREEN_WIDTH * TILE_WIDTH / 4, SCREEN_HEIGHT * TILE_WIDTH / 4);
	g_window->setView(view);

	while (window.isOpen())
	{
		sf::Event event;
		while (window.pollEvent(event))
		{
			if (event.type == sf::Event::Closed)
				window.close();
			if (event.type == sf::Event::KeyPressed) {
				int p_type = -1;
				switch (event.key.code) {
				case sf::Keyboard::Left:
					send_move_packet(MV_LEFT);
					break;
				case sf::Keyboard::Right:
					send_move_packet(MV_RIGHT);
					break;
				case sf::Keyboard::Up:
					send_move_packet(MV_UP);
					break;
				case sf::Keyboard::Down:
					send_move_packet(MV_DOWN);
					break;
				case sf::Keyboard::Space:
					send_attack_packet();
					break;
				case sf::Keyboard::Escape:
					window.close();
					break;
				}
			}
		}

		window.clear();
		client_main();
		window.display();
	}

	cout << "클라이언트 종료" << endl;
	send_logout_packet();
	client_finish();

	return 0;
}