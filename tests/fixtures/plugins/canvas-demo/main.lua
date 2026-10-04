local c = helix.canvas("c")
local sz = helix.subject.string("sz", "")
local n = helix.subject.int("n", 0)
local mode = helix.subject.int("mode", 0)

c:line(0, 0, 10, 10, {color = "primary"})
c:commit()

c:on_size(function(w, h)
  n:set(n:get() + 1)
  sz:set(w .. "x" .. h)
  if mode:get() == 1 then c:on_size(nil) end
end)
