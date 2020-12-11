#include "Default.h"


class Astar
{
public: 
	class Coordinate // x, y 좌표 클래스
	{
	public:
		int x;
		int y;
	public:
		void Set(int _x=0, int _y=0)
		{
			x = _x; y = _y;
		}
	public:
		Coordinate() {}
		Coordinate(int _x, int _y) : x(_x), y(_y) {}
	};

	class Node // 노드 클래스
	{
	public:
		Coordinate point;
		int F, G, H; // F = 비용, G = 지난 거리, H = 남은 거리

		Coordinate end;
		Node* pParent;

	public:
		Node(int _x, int _y, Node* _pParent, Coordinate _EndPoint);
		Node();
		~Node();
	};

	class Map // 맵 클래스
	{			
	public:
		int sizeX, sizeY;
		int** map;
	public:
		void Copy(Map* _map); 
		void PrintMap();
	public:
		Map();
		~Map();
	};



private: // 내부 함수
	list<Coordinate*> FindPath(Map* Navi, Coordinate StartPoint, Coordinate EndPoint);
	list<Node*>::iterator FindNextNode(list<Node*>* pOpenNode); 
	list<Node*>::iterator FindCoordNode(int x, int y, list<Node*>* NodeList); 
	void ExploreNode(Map* Navi, Node* SNode, list<Node*>* OpenNode, list<Node*>* CloseNode, Coordinate EndPoint); 
	
public:
	void FindPath();
	Coordinate GetPos(int order); 
	list<Coordinate*> GetPath() { return path; } 
	void SetFree(int _x, int _y);
	void SetObstacle(int _x, int _y); 
	void PrintPath();
	void PrintMap();
	void PrintNavi();

private:
	Map Navi; // 맵 생성
	Map printNavi; 

private:
	Coordinate StartPoint; // 출발지점
	Coordinate EndPoint; // 목표지점
	list<Coordinate*> path; 
	list<Coordinate*>::iterator iter; 

public:
	Astar(Coordinate _StartPoint, Coordinate _EndPoint) 
	{
		StartPoint.x = _StartPoint.x; StartPoint.y = _StartPoint.y;
		EndPoint.x = _EndPoint.x; EndPoint.y = _EndPoint.y;
		FindPath();
	}
	~Astar() 
	{ 
		iter = path.begin(); 
		for (; iter != path.end(); iter++)
		{
			delete *iter;
		}
	}
};

