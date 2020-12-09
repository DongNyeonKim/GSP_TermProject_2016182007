myid = 99999;
my_x=-1;
my_y=-1;
my_level = -1;
my_hp = -1;
move_type = false;
attack_type = false;


count=0;
is_meet = false;
p_id=null;

function set_uid(x)
	myid = x;
end

function set_x(x)
	my_x = x;
end

function set_y(x)
	my_y = x;
end

function set_level(x)
	my_level = x;
end

function set_hp(x)
	my_hp = x;
end

function set_move_type(x)
	move_type = x;
end

function set_attack_type(x)
	attack_type = x;
end

function count_move()
	if(is_meet==true) then
		count = count + 1;
	end
	if(count==3) then
		npc_bye();
	end
end

function npc_bye()
	API_SendMessage(myid, p_id, "Bye");
	count=0;
	is_meet=false;
end

function event_player_move(player)
	player_x = API_get_x(player);
	player_y = API_get_y(player);
	my_x = API_get_x(myid);
	my_y = API_get_y(myid);
	if (player_x == my_x) then
		if (player_y == my_y) then
			API_SendMessage(myid, player, "HELLO");
			p_id =player;
			count=0;
			is_meet = true;
		end
	end
end


