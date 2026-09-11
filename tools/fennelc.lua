-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Drive the vendored Fennel library (scripts/core/fennel.lua) as a compiler.
-- Usage:
--   luajit fennelc.lua <fennel.lua> [--use-bit-lib] --compile <input.fnl> <output.lua>

local fennel_lua = arg[1]
if not fennel_lua then
  io.stderr:write("usage: fennelc.lua <fennel.lua> [--use-bit-lib] --compile <input.fnl> <output.lua>\n")
  os.exit(1)
end

local use_bit_lib = false
local compile_src, compile_dst
local i = 2
while i <= #arg do
  local a = arg[i]
  if a == "--use-bit-lib" then
    use_bit_lib = true
  elseif a == "--compile" then
    compile_src = arg[i + 1]
    compile_dst = arg[i + 2]
    i = i + 2
  else
    io.stderr:write("fennelc.lua: unknown argument: " .. tostring(a) .. "\n")
    os.exit(1)
  end
  i = i + 1
end

if not compile_src or not compile_dst then
  io.stderr:write("fennelc.lua: --compile <input.fnl> <output.lua> is required\n")
  os.exit(1)
end

local fennel = assert(loadfile(fennel_lua))()
local src_file = assert(io.open(compile_src, "rb"))
local source = src_file:read("*a")
src_file:close()

local compiled = fennel.compileString(source, {
  filename = compile_src,
  useBitLib = use_bit_lib,
})

local dst_file = assert(io.open(compile_dst, "wb"))
dst_file:write(compiled)
if compiled:sub(-1) ~= "\n" then
  dst_file:write("\n")
end
dst_file:close()
