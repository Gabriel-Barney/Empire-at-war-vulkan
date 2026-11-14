-- ==== s78m t06: Tyber Zann Tactical_Bribe probe (tools/probe_mod/s78m_bribe/s78m_bribe.lua) ====
-- build_s78m_mod.py appends, to the shipped BusyTacticalFreeStore.lua, a generated S8_CFG table and this file.
--
-- Questions (docs/re/TACTICAL_COMBAT.md sec. 5.4.1, roadmap s78m t05 / t06):
--   cost   credits paid for a bribe of a Stormtrooper_Team (port: Tactical_Bribe_Cost 25 x the user's
--          Bribe_Modifier_On_Uncorrupted_Planets 1.0 = 25; Get_Bribe_Cost(tyber) is read too)
--   team   whether the whole team (container + every member) changes owner or only the clicked trooper; a second
--          team of the same company is the control
--   timing frames from the order to the payment and to each owner change (port: one frame, no animation delay)
--   cool   the recharge (port: Recharge_Seconds 50 = 1500 frames from the payment)
--   corners container as the target, out-of-range start (Tyber closes in; range on container or members), and a
--          refused bribe (too few credits: paid? converted? recharge started? order kept until money returns?)
--
-- Arming: only the free store of the AI player whose faction is S8_CFG.ai_faction (EMPIRE) runs it, in Land mode,
-- and only when Find_Player(S8_CFG.human_faction) (Underworld) is human, so the main-menu background battle never
-- arms it. ServiceRate is lowered to 0.05 s: the probe runs on every free-store service (every 3rd frame); while it
-- runs the shipped FreeStoreService / On_Unit_Service bodies are skipped.
-- Output: Lua strings "S8M|<KIND>|sid=<script id>|n=<n>|f=<mode frame>|...|END" kept in S8_LOG; s78m_run.py finds
-- them with a read-only heap scan. f = GetCurrentTime() * 30 rounded.
-- Retail rules kept here (earlier probe runs): Lua 5.0 (no '#', no '%', no math library); every engine call on an
-- object is gated by Is_Valid (a failed engine read stops a retail script silently, pcall cannot catch it);
-- Activate_Ability always gets its second argument; Game_Message (not _MessagePopup) for the driver's cues.

S8_LOG = {}
S8_N = 0
S8_SID = string.sub((string.gsub(tostring(S8_LOG), "[^%w]", "")), -6)
S8 = { on = false, done = false, f = -1, mode = "?", fac = "?", tries = 0, pending = false, old_rate = nil,
	trace = false, hb = nil }
S8_API_BAD = {}
S8_MAX_RECORDS = 20000

if S8_CFG == nil then
	S8_CFG = { session = "?", ai_faction = "?", human_faction = "?" }
end

-- ---- records ---------------------------------------------------------------------------------------------------

function S8_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function S8_Frame()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return tonumber(string.format("%.0f", t * 30))
	end
	return nil
end

function S8_Rec(kind, fields)
	pcall(function()
		if S8_N >= S8_MAX_RECORDS and kind ~= "DONE" then
			return
		end
		S8_N = S8_N + 1
		local body = fields or ""
		if string.len(body) > 420 then
			body = string.sub(body, 1, 420) .. "~"
		end
		local f = S8_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		S8_LOG[S8_N] = "S8M|" .. kind .. "|sid=" .. S8_SID .. "|n=" .. S8_N .. "|f=" .. fs .. "|" .. body .. "|END"
	end)
end

-- one record before each risky engine call while S8.trace is on (finds a call that stops the script silently)
function S8_Step(what)
	if S8.trace then
		S8_Rec("ST", "s=" .. what)
	end
end

function S8_ApiFail(tag, err)
	if not S8_API_BAD[tag] then
		S8_API_BAD[tag] = true
		S8_Rec("APIERR", "api=" .. S8_Str(tag) .. "|err=" .. string.sub(S8_Str(err), 1, 150))
	end
end

function S8_Say(id)
	S8_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

-- ---- engine reads (all gated) -----------------------------------------------------------------------------------

function S8_Valid(o)
	if o == nil then
		return false
	end
	local ok, v = pcall(function() return o.Is_Valid() end)
	return ok and v == true
end

function S8_Id(o)
	-- Get_Object_ID (0x5791f0, a number); Get_ID belongs to PlayerWrapper (retail run 20261108-214221 read "?" for
	-- every object through it). "?" when unreadable: callers then key objects by a local label.
	if not S8_Valid(o) then
		return "?"
	end
	local id = nil
	pcall(function() id = o.Get_Object_ID() end)
	if id == nil then
		return "?"
	end
	return S8_Str(id)
end

function S8_Type(o)
	if not S8_Valid(o) then
		return "?"
	end
	local n = "?"
	pcall(function() n = o.Get_Type().Get_Name() end)
	return S8_Str(n)
end

-- x, y of an object (numbers) or nil
function S8_XYn(o)
	if not S8_Valid(o) then
		return nil
	end
	local ok, x, y = pcall(function()
		local a, b, c = o.Get_Position().Get_XYZ()
		if type(a) == "table" then
			return a[1], a[2]
		end
		return a, b
	end)
	if ok and type(x) == "number" and type(y) == "number" then
		return x, y
	end
	return nil
end

function S8_XY(o)
	local x, y = S8_XYn(o)
	if x then
		return string.format("%.1f,%.1f", x, y)
	end
	return "?"
end

-- faction name of the owner (upper case), "dead" for an invalid object, "?" when unreadable
function S8_Owner(o)
	if not S8_Valid(o) then
		return "dead"
	end
	local n = "?"
	pcall(function() n = string.upper(tostring(o.Get_Owner().Get_Faction_Name())) end)
	return S8_Str(n)
end

function S8_Flag(o, method, arg)
	if not S8_Valid(o) then
		return "dead"
	end
	local ok, v = pcall(function() return o[method](arg) end)
	if ok then
		if v then
			return "1"
		end
		return "0"
	end
	S8_ApiFail(method, v)
	return "?"
end

function S8_Call(o, tag, f)
	if not S8_Valid(o) then
		return "dead"
	end
	S8_Step(tag)
	local ok, err = pcall(f)
	if ok then
		return "ok"
	end
	S8_ApiFail(tag, err)
	return "err"
end

function S8_Pos(x, y, z)
	local ok, p = pcall(Create_Position, x, y, z or 0)
	if ok then
		return p
	end
	S8_ApiFail("Create_Position", p)
	return nil
end

function S8_Player(name)
	local p = nil
	pcall(function() p = Find_Player(name) end)
	return p
end

function S8_Credits()
	local p = S8_Player(S8_CFG.human_faction)
	if p == nil then
		return nil
	end
	local ok, c = pcall(function() return p.Get_Credits() end)
	if ok and type(c) == "number" then
		return c
	end
	return nil
end

function S8_Num(v)
	if type(v) == "number" then
		return string.format("%.2f", v)
	end
	return "?"
end

function S8_FindAll(tname)
	local ok, r = pcall(Find_All_Objects_Of_Type, tname)
	if ok and type(r) == "table" then
		return r
	end
	if not ok then
		S8_ApiFail("Find_All_Objects_Of_Type:" .. tname, r)
	end
	return {}
end

-- Spawn_Unit of type tname at pos for player. Returns the result table (may be empty) and how.
function S8_SpawnList(tname, pos, player)
	local ty = nil
	pcall(function() ty = Find_Object_Type(tname) end)
	if ty == nil then
		return {}, "no_type"
	end
	if pos == nil or player == nil then
		return {}, "no_pos_or_player"
	end
	S8_Step("Spawn_Unit:" .. tname)
	local ok, r = pcall(Spawn_Unit, ty, pos, player)
	if ok and type(r) == "table" then
		return r, "spawn_unit"
	end
	if ok and r ~= nil and S8_Valid(r) then
		return { r }, "spawn_unit_single"
	end
	if not ok then
		S8_ApiFail("Spawn_Unit:" .. tname, r)
	end
	return {}, "spawn_failed"
end

function S8_Num_Id(id)
	local n = tonumber(id)
	if n == nil then
		return 1e18
	end
	return n
end

-- ---- the probe ------------------------------------------------------------------------------------------------------

S8B = { phase = "init", tyber = nil, ti = 0, cur = nil, next_f = nil, seen = {} }

function S8B_Rel(dx, dy)
	-- relative to Tyber's current position (he may have walked in an earlier trial), else to the site
	local s = S8_CFG.site
	local x, y = S8_XYn(S8B.tyber)
	if x == nil then
		x, y = s.x, s.y
	end
	return S8_Pos(x + dx, y + dy, s.z)
end

function S8B_SpawnTyber(F)
	local cfg = S8_CFG
	local human = S8_Player(cfg.human_faction)
	local site = S8_Pos(cfg.site.x, cfg.site.y, cfg.site.z)
	local how = "none"
	local t = nil
	for _, name in ipairs({ cfg.hero, cfg.hero_alt }) do
		local r, h = S8_SpawnList(name, site, human)
		how = name .. ":" .. h
		for _, o in ipairs(r) do
			if t == nil and string.upper(S8_Type(o)) == string.upper(cfg.hero) then
				t = o
			end
		end
		if t == nil then
			-- a company spawn may return the transport / company; look the hero up by type near the site
			for _, o in ipairs(S8_FindAll(cfg.hero)) do
				if t == nil and S8_Valid(o) and S8_Owner(o) == string.upper(cfg.human_faction) then
					t = o
				end
			end
		end
		if t ~= nil then
			break
		end
	end
	S8B.tyber = t
	if t == nil then
		S8_Rec("ERR", "what=no_tyber|how=" .. S8_Str(how))
		return false
	end
	local af0 = S8_Flag(t, "Is_Ability_Autofire", cfg.ability)
	S8_Call(t, "Make_Invulnerable", function() t.Make_Invulnerable(true) end)
	-- the shipped AI (AI_PLAN_UNDERWORLD_TYBERMANAGERLAND.LUA) turns the bribe's autofire off the same way
	local afc = S8_Call(t, "Set_Single_Ability_Autofire", function() t.Set_Single_Ability_Autofire(cfg.ability, false) end)
	S8_Rec("TYBER", "id=" .. S8_Id(t) .. "|type=" .. S8_Type(t) .. "|how=" .. S8_Str(how) .. "|owner=" .. S8_Owner(t) ..
		"|pos=" .. S8_XY(t) .. "|site=" .. string.format("%.0f,%.0f", cfg.site.x, cfg.site.y) .. "|autofire_before=" ..
		af0 .. "|autofire_set=" .. afc .. "|autofire_after=" .. S8_Flag(t, "Is_Ability_Autofire", cfg.ability) ..
		"|rdy=" .. S8_Flag(t, "Is_Ability_Ready", cfg.ability) .. "|act=" .. S8_Flag(t, "Is_Ability_Active", cfg.ability) ..
		"|cr=" .. S8_Num(S8_Credits()))
	return true
end

-- ids of every existing member / container object (so the next spawn's objects are told apart when ids are readable;
-- otherwise the position filter of S8B_Identify does it: earlier trials' troopers are despawned)
function S8B_Snapshot()
	local s = {}
	for _, tn in ipairs({ S8_CFG.member_type, S8_CFG.team_type }) do
		for _, o in ipairs(S8_FindAll(tn)) do
			local id = S8_Id(o)
			if id ~= "?" then
				s[id] = true
			end
		end
	end
	return s
end

function S8B_StartTrial(F, tc, label)
	local t = { k = tc.k, name = tc.name, cfg = tc, f0 = F, phase = "settle", old = S8B_Snapshot(), teams = {},
		order = {}, objs = {}, last = {}, conv_f = nil, label = label, errs = 0, ret_objs = {} }
	S8B.cur = t
	local squad = S8_CFG.squad
	local pos = S8B_Rel(tc.dx, 0)
	t.sx, t.sy = S8_XYn(S8B.tyber)
	if t.sx == nil then
		t.sx, t.sy = S8_CFG.site.x, S8_CFG.site.y
	end
	t.sx = t.sx + tc.dx
	local r, how = S8_SpawnList(squad, pos, S8_Player(S8_CFG.ai_faction))
	if table.getn(r) == 0 then
		for _, alt in ipairs(S8_CFG.squad_alt) do
			if table.getn(r) == 0 then
				squad = alt
				r, how = S8_SpawnList(alt, pos, S8_Player(S8_CFG.ai_faction))
			end
		end
	end
	t.squad = squad
	t.ret_objs = r
	local s = ""
	for i, o in ipairs(r) do
		s = s .. S8_Type(o) .. ":" .. S8_Id(o) .. "@" .. S8_XY(o) .. ";"
	end
	S8_Rec("TRIAL", "k=" .. t.k .. "|name=" .. t.name .. "|target=" .. tc.target .. "|dx=" .. tc.dx .. "|squad=" ..
		squad .. "|how=" .. how .. "|ret=" .. string.sub(s, 1, 240) .. "|tyber=" .. S8_XY(S8B.tyber) .. "|cr=" ..
		S8_Num(S8_Credits()) .. "|rdy=" .. S8_Flag(S8B.tyber, "Is_Ability_Ready", S8_CFG.ability))
end

-- is o (a candidate member) new, the AI's, and near this trial's spawn point?
function S8B_IsNew(t, o, ai)
	if not S8_Valid(o) or S8_Owner(o) ~= ai then
		return false
	end
	local id = S8_Id(o)
	if id ~= "?" and t.old[id] then
		return false
	end
	local x, y = S8_XYn(o)
	if x == nil then
		return false
	end
	local r = S8_CFG.near
	return (x - t.sx) * (x - t.sx) + (y - t.sy) * (y - t.sy) <= r * r
end

-- after the settle time: find the new members, group them by Get_Parent_Object (the team container). Object keys:
-- Get_Object_ID when readable, else local labels (M<n> members, T<n> teams; a parent wrapper is matched by equality,
-- then by its exact position, since a wrapper may not be cached).
function S8B_Identify(t, F)
	local ai = string.upper(S8_CFG.ai_faction)
	local byteam = {}
	local keys = {}
	local parents = {}
	local cands = {}
	local src = "type"
	for _, o in ipairs(S8_FindAll(S8_CFG.member_type)) do
		if S8B_IsNew(t, o, ai) then
			table.insert(cands, o)
		end
	end
	if table.getn(cands) == 0 then
		-- no member by type: every AI object near the spawn point that has a team parent
		src = "player"
		local ok, all = pcall(Find_All_Objects_Of_Type, S8_Player(S8_CFG.ai_faction))
		if ok and type(all) == "table" then
			for _, o in ipairs(all) do
				if S8B_IsNew(t, o, ai) then
					local p = nil
					pcall(function() p = o.Get_Parent_Object() end)
					if S8_Valid(p) then
						table.insert(cands, o)
					end
				end
			end
		end
	end
	local nm = 0
	for _, o in ipairs(cands) do
		local id = S8_Id(o)
		nm = nm + 1
		if id == "?" then
			id = "M" .. t.k .. "_" .. nm
		end
		local p = nil
		S8_Step("Get_Parent_Object")
		pcall(function() p = o.Get_Parent_Object() end)
		local pid = nil
		if S8_Valid(p) then
			local pxy = S8_XY(p)
			for _, q in ipairs(parents) do
				if pid == nil and (q.o == p or (pxy ~= "?" and q.xy == pxy)) then
					pid = q.key
					p = q.o
				end
			end
			if pid == nil then
				pid = S8_Id(p)
				if pid == "?" then
					pid = "T" .. t.k .. "_" .. (table.getn(parents) + 1)
				end
				table.insert(parents, { o = p, key = pid, xy = pxy })
			end
		else
			p = nil
			pid = "solo" .. id
		end
		if byteam[pid] == nil then
			byteam[pid] = { cid = pid, c = p, members = {} }
			table.insert(keys, pid)
		end
		table.insert(byteam[pid].members, { o = o, id = id })
	end
	-- spawned team objects no member pointed at (recorded only)
	local extra = ""
	for _, o in ipairs(t.ret_objs) do
		local hit = false
		for _, q in ipairs(parents) do
			if q.o == o or (q.xy ~= "?" and q.xy == S8_XY(o)) then
				hit = true
			end
		end
		if not hit then
			extra = extra .. S8_Type(o) .. ":" .. S8_Id(o) .. "@" .. S8_XY(o) .. ";"
		end
	end
	S8_Rec("IDENT", "k=" .. t.k .. "|src=" .. src .. "|members=" .. table.getn(cands) .. "|teams=" .. table.getn(keys) ..
		"|spawn=" .. string.format("%.0f,%.0f", t.sx, t.sy) .. "|unmatched_ret=" .. string.sub(extra, 1, 200))
	table.sort(keys, function(a, b) return S8_Num_Id(a) < S8_Num_Id(b) end)
	local roles = { "A", "B", "C", "D", "E", "F" }
	for i, key in ipairs(keys) do
		local tm = byteam[key]
		table.sort(tm.members, function(a, b) return S8_Num_Id(a.id) < S8_Num_Id(b.id) end)
		tm.role = roles[i] or ("X" .. i)
		table.insert(t.order, tm)
		local ms = ""
		for _, m in ipairs(tm.members) do
			ms = ms .. m.id .. ";"
			table.insert(t.objs, { o = m.o, id = m.id, team = tm.role, kind = "m" })
		end
		if tm.c then
			table.insert(t.objs, { o = tm.c, id = tm.cid, team = tm.role, kind = "c" })
		end
		S8_Rec("TEAM", "k=" .. t.k .. "|role=" .. tm.role .. "|cid=" .. tm.cid .. "|ctype=" .. S8_Type(tm.c) ..
			"|cowner=" .. S8_Owner(tm.c) .. "|cpos=" .. S8_XY(tm.c) .. "|n=" .. table.getn(tm.members) .. "|mtype=" ..
			S8_Type(tm.members[1].o) .. "|members=" .. string.sub(ms, 1, 200))
	end
	t.teamA = t.order[1]
	if t.teamA == nil then
		S8_Rec("ERR", "what=no_team|k=" .. t.k)
		return false
	end
	-- every spawned trooper: no AI use. Control teams (B, C, ...): invulnerable and not firing, so they stay intact
	-- and leave Tyber alone; the target team is left as spawned (vulnerable, firing at the invulnerable Tyber).
	for _, x in ipairs(t.objs) do
		local o = x.o
		S8_Call(o, "Prevent_AI_Usage", function() o.Prevent_AI_Usage(true) end)
		if x.team ~= "A" then
			S8_Call(o, "Make_Invulnerable", function() o.Make_Invulnerable(true) end)
			S8_Call(o, "Prevent_All_Fire", function() o.Prevent_All_Fire(true) end)
		end
	end
	for _, x in ipairs(t.objs) do
		t.last[x.id] = S8_Owner(x.o)
	end
	return true
end

function S8B_Counts(t)
	-- per team: container owner and member owners, e.g. "A:c=EMPIRE,m=EMPIRE9"
	local s = ""
	for _, tm in ipairs(t.order) do
		local cnt = {}
		local keys = {}
		for _, m in ipairs(tm.members) do
			local ow = S8_Owner(m.o)
			if cnt[ow] == nil then
				cnt[ow] = 0
				table.insert(keys, ow)
			end
			cnt[ow] = cnt[ow] + 1
		end
		table.sort(keys)
		local ms = ""
		for _, k in ipairs(keys) do
			ms = ms .. k .. cnt[k] .. "+"
		end
		local co = "none"
		if tm.c then
			co = S8_Owner(tm.c)
		end
		s = s .. tm.role .. ":c=" .. co .. ",m=" .. ms .. ";"
	end
	return s
end

-- the nearest live member of team A (squared XY distance to Tyber, no math library)
function S8B_Nearest(t)
	local tx, ty = S8_XYn(S8B.tyber)
	if tx == nil then
		return "?", "?"
	end
	local best, bid = nil, "?"
	for _, m in ipairs(t.teamA.members) do
		local x, y = S8_XYn(m.o)
		if x then
			local d2 = (x - tx) * (x - tx) + (y - ty) * (y - ty)
			if best == nil or d2 < best then
				best, bid = d2, m.id
			end
		end
	end
	if best == nil then
		return "?", "?"
	end
	return string.format("%.1f", best), bid
end

function S8B_Poll(t, F, full)
	local ab = S8_CFG.ability
	local ty = S8B.tyber
	local cr = S8_Credits()
	local crs = S8_Num(cr)
	if t.last_cr ~= nil and crs ~= t.last_cr then
		local d = "?"
		if cr ~= nil and t.last_crn ~= nil then
			d = string.format("%.2f", cr - t.last_crn)
		end
		S8_Rec("CR", "k=" .. t.k .. "|from=" .. t.last_cr .. "|to=" .. crs .. "|d=" .. d)
	end
	t.last_cr = crs
	t.last_crn = cr
	local rdy = S8_Flag(ty, "Is_Ability_Ready", ab)
	if t.last_rdy ~= nil and rdy ~= t.last_rdy then
		S8_Rec("RDY", "k=" .. t.k .. "|from=" .. t.last_rdy .. "|to=" .. rdy)
	end
	t.last_rdy = rdy
	local act = S8_Flag(ty, "Is_Ability_Active", ab)
	if t.last_act ~= nil and act ~= t.last_act then
		S8_Rec("ACTV", "k=" .. t.k .. "|from=" .. t.last_act .. "|to=" .. act)
	end
	t.last_act = act
	local human = string.upper(S8_CFG.human_faction)
	local conv = false
	for _, x in ipairs(t.objs) do
		local ow = S8_Owner(x.o)
		if t.last[x.id] ~= nil and ow ~= t.last[x.id] then
			S8_Rec("OWN", "k=" .. t.k .. "|id=" .. x.id .. "|team=" .. x.team .. "|ok=" .. x.kind .. "|from=" ..
				t.last[x.id] .. "|to=" .. ow .. "|pos=" .. S8_XY(x.o))
		end
		t.last[x.id] = ow
		if x.team == "A" and ow == human then
			conv = true
		end
	end
	if conv and t.conv_f == nil then
		t.conv_f = F
		S8_Rec("CONV", "k=" .. t.k .. "|cr=" .. crs .. "|tyber=" .. S8_XY(ty) .. "|counts=" .. S8B_Counts(t))
	end
	if full then
		local nm2, nid = S8B_Nearest(t)
		local tgt = "?"
		if t.target then
			tgt = S8_XY(t.target)
		end
		local ctr = "?"
		if t.teamA.c then
			ctr = S8_XY(t.teamA.c)
		end
		S8_Rec("P", "k=" .. t.k .. "|ph=" .. t.phase .. "|cr=" .. crs .. "|rdy=" .. rdy .. "|act=" .. act .. "|ty=" ..
			S8_XY(ty) .. "|tgt=" .. tgt .. "|ctr=" .. ctr .. "|nm2=" .. nm2 .. "|nid=" .. nid .. "|cnt=" .. S8B_Counts(t))
	end
end

function S8B_Money(t, amount, why)
	local p = S8_Player(S8_CFG.human_faction)
	local before = S8_Credits()
	local ok = "err"
	if p ~= nil then
		S8_Step("Give_Money")
		local okc, err = pcall(function() p.Give_Money(amount) end)
		if okc then
			ok = "ok"
		else
			S8_ApiFail("Give_Money", err)
		end
	end
	S8_Rec("MONEY", "k=" .. t.k .. "|why=" .. why .. "|amount=" .. S8_Num(amount) .. "|call=" .. ok .. "|before=" ..
		S8_Num(before) .. "|after=" .. S8_Num(S8_Credits()))
end

function S8B_Activate(t, F, target, kind)
	local ab = S8_CFG.ability
	local ty = S8B.tyber
	local rdy = S8_Flag(ty, "Is_Ability_Ready", ab)
	local ret = "dead_target"
	if S8_Valid(target) and S8_Valid(ty) then
		S8_Step("Activate_Ability:" .. kind)
		local ok, r = pcall(function() return ty.Activate_Ability(ab, target) end)
		if not ok then
			S8_ApiFail("Activate_Ability:" .. kind, r)
			ret = "error"
		elseif r == nil then
			ret = "nil"
		elseif r == false then
			ret = "false"
		elseif type(r) == "userdata" or type(r) == "table" then
			ret = "block"
		else
			ret = S8_Str(r)
		end
	end
	S8_Rec("ACT", "k=" .. t.k .. "|ak=" .. kind .. "|tid=" .. S8_Id(target) .. "|ttype=" .. S8_Type(target) ..
		"|ret=" .. ret .. "|rdy_before=" .. rdy .. "|rdy_after=" .. S8_Flag(ty, "Is_Ability_Ready", ab) ..
		"|act_after=" .. S8_Flag(ty, "Is_Ability_Active", ab) .. "|cr=" .. S8_Num(S8_Credits()) .. "|tyber=" ..
		S8_XY(ty) .. "|tpos=" .. S8_XY(target))
	return ret
end

-- Get_Bribe_Cost(tyber) of the team, member and target types (the shipped Tyber AI's own cost call); one record
-- per reading, each preceded by a step record so a silent stop shows which call it was
function S8B_Costs(t)
	local ty = S8B.tyber
	if not S8_Valid(ty) then
		return
	end
	local was = S8.trace
	S8.trace = true
	local names = { S8_CFG.team_type, S8_CFG.member_type, "Stormtrooper" }
	for _, n in ipairs(names) do
		local tp = nil
		pcall(function() tp = Find_Object_Type(n) end)
		local v = "no_type"
		if tp ~= nil then
			S8_Step("Get_Bribe_Cost:" .. n)
			local ok, c = pcall(function() return tp.Get_Bribe_Cost(ty) end)
			if ok and type(c) == "number" then
				v = string.format("%.3f", c)
			elseif ok then
				v = "ret_" .. type(c)
			else
				S8_ApiFail("Get_Bribe_Cost:" .. n, c)
				v = "error"
			end
		end
		S8_Rec("COST", "k=" .. t.k .. "|type=" .. n .. "|via=Find_Object_Type|cost=" .. v)
	end
	-- the type of the target object itself (the AI reads bribe_target.Get_Game_Object().Get_Type())
	for _, x in ipairs({ { o = t.target, w = "target" }, { o = t.teamA.c, w = "container" } }) do
		if S8_Valid(x.o) then
			local v = "?"
			S8_Step("Get_Bribe_Cost:" .. x.w)
			local ok, c = pcall(function() return x.o.Get_Type().Get_Bribe_Cost(ty) end)
			if ok and type(c) == "number" then
				v = string.format("%.3f", c)
			elseif not ok then
				S8_ApiFail("Get_Bribe_Cost:" .. x.w, c)
				v = "error"
			end
			S8_Rec("COST", "k=" .. t.k .. "|type=" .. S8_Type(x.o) .. "|via=" .. x.w .. "|cost=" .. v)
		end
	end
	S8.trace = was
end

function S8B_Tick(F)
	local cfg = S8_CFG
	if S8B.phase == "init" then
		if S8B.next_f == nil then
			S8B.next_f = cfg.start_frame
			S8_Say("TEXT_S78M_ARMED")
		end
		if F >= S8B.next_f then
			if not S8B_SpawnTyber(F) then
				S8_Say("TEXT_S78M_DONE")
				S8_Finish("why=no_tyber")
				return
			end
			S8B.phase = "pause"
			S8B.next_f = F + cfg.gap
		end
		return
	end
	if S8B.phase == "pause" then
		if F >= S8B.next_f then
			S8B.ti = S8B.ti + 1
			local tc = cfg.trials[S8B.ti]
			if tc == nil then
				if cfg.manual and cfg.manual.on then
					S8B.phase = "manual"
					S8B_StartTrial(F, cfg.manual, "manual")
					S8_Say("TEXT_S78M_MANUAL")
				else
					S8_Say("TEXT_S78M_DONE")
					S8_Finish("trials=" .. (S8B.ti - 1))
				end
				return
			end
			S8B.phase = "trial"
			S8B_StartTrial(F, tc, "scripted")
		end
		return
	end
	local t = S8B.cur
	if t == nil then
		return
	end
	-- each trial is independent: an error inside it is recorded (first 3), and after 3 errors, or when the trial runs
	-- past its time limit, it ends (TEND aborted=...) and the next trial starts
	if t.phase ~= "end" then
		local ok, err = pcall(S8B_TrialTick, t, F)
		if not ok then
			t.errs = (t.errs or 0) + 1
			if t.errs <= 3 then
				S8_Rec("ERR", "where=trial|k=" .. t.k .. "|ph=" .. tostring(t.phase) .. "|err=" .. string.sub(S8_Str(err), 1, 200))
			end
			if t.errs >= 3 then
				t.phase = "end"
				t.aborted = "errors"
			end
		end
		local limit = cfg.settle + (t.cfg.watch or 0) + cfg.cooldown_max + 600
		if t.phase ~= "end" and F - t.f0 > limit then
			t.phase = "end"
			t.aborted = "timeout"
		end
	end
	if t.phase == "end" then
		S8B_EndTrial(t, F)
	end
end

function S8B_TrialTick(t, F)
	local cfg = S8_CFG
	local tc = t.cfg
	local rel = F - t.f0
	if t.phase == "settle" then
		if rel >= cfg.settle then
			if not S8B_Identify(t, F) then
				t.phase = "end"
			else
				t.phase = "act"
			end
		end
		return
	end
	if t.phase == "act" then
		if t.teamA == nil then
			t.phase = "end"
			return
		end
		S8B_Poll(t, F, true)
		t.target = nil
		for _, x in ipairs(t.teamA.members) do
			if t.target == nil and S8_Valid(x.o) then
				t.target = x.o
			end
		end
		if tc.target == "container" and t.teamA.c then
			t.target = t.teamA.c
		end
		local nm2, nid = S8B_Nearest(t)
		S8_Rec("PRE", "k=" .. t.k .. "|target=" .. tc.target .. "|tid=" .. S8_Id(t.target) .. "|cid=" .. t.teamA.cid ..
			"|cr=" .. S8_Num(S8_Credits()) .. "|rdy=" .. S8_Flag(S8B.tyber, "Is_Ability_Ready", cfg.ability) ..
			"|autofire=" .. S8_Flag(S8B.tyber, "Is_Ability_Autofire", cfg.ability) .. "|ty=" .. S8_XY(S8B.tyber) ..
			"|tgt=" .. S8_XY(t.target) .. "|ctr=" .. S8_XY(t.teamA.c) .. "|nm2=" .. nm2 .. "|cnt=" .. S8B_Counts(t))
		if tc.manual then
			-- by hand: the driver selects Tyber and bribes; only watch (camera call last: it is the riskiest)
			t.phase = "watch"
			t.fact = F
			S8_Rec("ST", "s=Point_Camera_At")
			pcall(Point_Camera_At, S8B.tyber)
			return
		end
		if tc.poor ~= nil then
			local c = S8_Credits()
			if c ~= nil then
				t.money_back = c - tc.poor
				S8B_Money(t, tc.poor - c, "poor")
			end
			-- the poll baseline is the credit after the Give_Money, so the next CR edge is the bribe's (or income)
			t.last_cr = S8_Num(S8_Credits())
			t.last_crn = S8_Credits()
		end
		t.fact = F
		-- no new baseline after the call: a payment made inside Activate_Ability shows as the next poll's CR edge
		-- (and in the ACT record's cr)
		t.ret = S8B_Activate(t, F, t.target, tc.target)
		t.phase = "watch"
		return
	end
	local ra = F - (t.fact or F)
	if t.phase == "watch" then
		local full = ra <= tc.fast or (ra - (t.lastp or -1000)) >= cfg.slow_every
		S8B_Poll(t, F, full)
		if full then
			t.lastp = ra
		end
		-- retry on a member: soon when the container order gave no block (refused), late when it was taken (Tyber
		-- may still be walking) but nothing changed owner
		local fb_at = tc.fallback_late
		if t.ret ~= "block" then
			fb_at = tc.fallback_after
		end
		if fb_at and not t.fb and t.conv_f == nil and ra >= fb_at then
			t.fb = true
			local m = nil
			for _, x in ipairs(t.teamA.members) do
				if m == nil and S8_Valid(x.o) then
					m = x.o
				end
			end
			if m then
				t.target = m
				S8B_Activate(t, F, m, "member_fallback")
			end
		end
		if t.money_back ~= nil and not t.restored and ra >= tc.restore_after then
			t.restored = true
			S8B_Money(t, t.money_back, "restore")
			t.last_cr = S8_Num(S8_Credits())
			t.last_crn = S8_Credits()
		end
		if tc.costs and t.conv_f ~= nil and not t.costs_done and F - t.conv_f >= 15 then
			t.costs_done = true
			S8B_Costs(t)
		end
		if ra >= tc.watch then
			t.phase = "cool"
			if t.label == "manual" then
				t.phase = "end"
			end
			t.fcool = F
			S8_Rec("WEND", "k=" .. t.k .. "|conv=" .. tostring(t.conv_f ~= nil) .. "|cnt=" .. S8B_Counts(t))
		end
		return
	end
	if t.phase == "cool" then
		-- wait for the bribe to be ready again (the recharge end is an RDY edge record), at most cooldown_max
		local rc = F - t.fcool
		S8B_Poll(t, F, (rc - (t.lastc or -1000)) >= cfg.slow_every)
		if (rc - (t.lastc or -1000)) >= cfg.slow_every then
			t.lastc = rc
		end
		local rdy = t.last_rdy
		if (rdy == "1" and rc >= 6) or rc >= cfg.cooldown_max or rdy == "dead" then
			t.phase = "end"
		end
		return
	end
end

function S8B_EndTrial(t, F)
	local cfg = S8_CFG
	S8_Rec("TEND", "k=" .. t.k .. "|name=" .. t.name .. "|conv_f=" .. tostring(t.conv_f) .. "|fact=" ..
		tostring(t.fact) .. "|rdy=" .. tostring(t.last_rdy) .. "|cr=" .. S8_Num(S8_Credits()) .. "|aborted=" .. tostring(t.aborted) .. "|cnt=" ..
		S8B_Counts(t))
	S8B.cur = nil
	-- remove this trial's troopers, so a converted team never fights the next trial's squad
	for _, x in ipairs(t.objs) do
		if S8_Valid(x.o) then
			pcall(function() x.o.Despawn() end)
		end
	end
	for _, o in ipairs(t.ret_objs or {}) do
		if S8_Valid(o) then
			pcall(function() o.Despawn() end)
		end
	end
	if t.label == "manual" then
		S8_Say("TEXT_S78M_DONE")
		S8_Finish("trials=" .. (S8B.ti - 1) .. "|manual=1")
		return
	end
	S8B.phase = "pause"
	S8B.next_f = F + cfg.gap
end

-- ---- arming and the service drivers -----------------------------------------------------------------------------

function S8_Arm()
	local fac, mode, human = "?", "?", true
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() human = PlayerObject.Is_Human() end)
	if (fac == "?" or fac == "NIL") and S8.tries < 200 then
		S8.pending = true
		return
	end
	S8.pending = false
	S8.mode = S8_Str(mode)
	S8.fac = S8_Str(fac)
	local human_ok = false
	pcall(function() human_ok = Find_Player(S8_CFG.human_faction).Is_Human() == true end)
	if mode == "Land" and not human_ok and S8.tries < 60 then
		S8.pending = true
		return
	end
	if mode == "Land" and fac == S8_CFG.ai_faction and human ~= true and human_ok and not S8_ARMED_ONCE then
		S8_ARMED_ONCE = true
		S8.on = true
		S8.old_rate = ServiceRate
		ServiceRate = 0.05
		S8_Rec("ARM", "fac=" .. S8.fac .. "|mode=" .. S8.mode .. "|session=" .. S8_Str(S8_CFG.session) .. "|rate=" ..
			S8_Str(S8.old_rate) .. "|tries=" .. S8.tries .. "|cr=" .. S8_Num(S8_Credits()))
	else
		S8_Rec("SKIP", "fac=" .. S8.fac .. "|mode=" .. S8.mode .. "|human=" .. tostring(human) .. "|human_side=" ..
			tostring(human_ok) .. "|session=" .. S8_Str(S8_CFG.session))
	end
end

function S8_Finish(extra)
	if S8.done then
		return
	end
	S8.done = true
	if S8.old_rate ~= nil then
		ServiceRate = S8.old_rate
	end
	local bad = ""
	for k, _ in pairs(S8_API_BAD) do
		bad = bad .. k .. ","
	end
	S8_Rec("DONE", "records=" .. S8_N .. "|apierr=" .. string.sub(S8_Str(bad), 1, 220) .. "|" .. (extra or ""))
end

function S8_Tick(src)
	if S8.pending then
		S8.tries = S8.tries + 1
		pcall(S8_Arm)
	end
	if not S8.on or S8.done then
		return
	end
	local F = S8_Frame()
	if F == nil or F == S8.f then
		return
	end
	S8.f = F
	if S8.hb == nil or F - S8.hb >= 30 then
		S8.hb = F
		S8_Rec("HB", "src=" .. src)
	end
	local ok, err = pcall(S8B_Tick, F)
	if not ok then
		-- each distinct message is recorded once (with its count so far); a probe that keeps failing stops itself
		local msg = string.sub(S8_Str(err), 1, 200)
		S8.errs = (S8.errs or 0) + 1
		if S8.errseen == nil then
			S8.errseen = {}
		end
		if S8.errseen[msg] == nil then
			S8.errseen[msg] = true
			S8_Rec("ERR", "where=tick|src=" .. src .. "|n_err=" .. S8.errs .. "|err=" .. msg)
		end
		if S8.errs >= 100 then
			S8_Say("TEXT_S78M_DONE")
			S8_Finish("why=tick_errors|n_err=" .. S8.errs)
		end
	end
end

function S8_Quiet()
	return S8.on and not S8.done
end

S8_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	local r = nil
	if S8_Orig_Base_Definitions then
		r = S8_Orig_Base_Definitions()
	end
	pcall(S8_Arm)
	return r
end

S8_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	pcall(S8_Tick, "fss")
	if S8_Quiet() or S8_Orig_FreeStoreService == nil then
		return
	end
	return S8_Orig_FreeStoreService()
end

S8_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(S8_Tick, "ous")
	if S8_Quiet() or S8_Orig_On_Unit_Service == nil then
		return
	end
	return S8_Orig_On_Unit_Service(object)
end

S8_Rec("LOAD", "session=" .. S8_Str(S8_CFG.session) .. "|ai_faction=" .. S8_Str(S8_CFG.ai_faction) .. "|human=" ..
	S8_Str(S8_CFG.human_faction))
-- ==== end of s78m_bribe.lua ====
