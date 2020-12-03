myid = 99999;
count=0;
is_meet = false;
p_id=null;

function set_uid(x)
	myid = x;
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


