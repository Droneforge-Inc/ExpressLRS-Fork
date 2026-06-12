-- Project-local Neovim config for ExpressLRS-Fork (PlatformIO)
-- Place this in src/.nvim.lua so it activates when you open files under src/.
--
-- First time: Neovim may prompt to trust the file. Run `:trust` (or answer yes).
-- You can also add to your options.lua:
--   vim.opt.exrc = true
--   vim.opt.secure = true   -- (default behavior for .nvim.lua is already quite safe)
--
-- Benefits:
--   * <leader>cb etc. do the right PIO thing instead of CMake
--   * :make runs the firmware build and populates quickfix with errors
--   * Easy commands to (re)generate compile_commands.json for clangd
--   * Local keymaps only affect buffers inside this project
--   * Visible in which-key under <leader>c (Code)

local floatterm = require("config.floatterm")

-- Try to get which-key for explicit registration (so buffer-local maps show in popup)
local wk_ok, wk = pcall(require, "which-key")

-- === Customize these for your workflow ===
local REGULATORY = "ISM_2400"                     -- or "EU_CE_2400"
local NIMBUS_ENV = "Unified_ESP32S3_2400_TX_via_UART"
local NATIVE_ENV = "native"
local PROJECT_DIR = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h")
local uv = vim.uv or vim.loop

local function pio_prefix()
  return string.format('PLATFORMIO_BUILD_FLAGS="-DRegulatory_Domain_%s"', REGULATORY)
end

local function build_cmd(env, target)
  local cmd = string.format('%s pio run -e %s', pio_prefix(), env)
  if target then
    cmd = cmd .. " -t " .. target
  end
  return cmd
end

local function project_cmd(cmd)
  return "cd " .. vim.fn.shellescape(PROJECT_DIR) .. " && " .. cmd
end

local function project_file(path)
  return PROJECT_DIR .. "/" .. path
end

local function db_present_at(path)
  local stat = uv.fs_stat(path)
  return stat and stat.type == "file" and stat.size > 0
end

local function restart_clangd()
  local clients = vim.lsp.get_clients({ name = "clangd" })
  for _, client in ipairs(clients) do
    vim.lsp.stop_client(client.id)
  end
  vim.defer_fn(function()
    vim.lsp.enable("clangd")
    vim.notify("clangd restarted (ELRS)", vim.log.levels.INFO)
  end, 80)
end

local function clangd_status()
  local clients = vim.lsp.get_clients({ bufnr = 0, name = "clangd" })
  local root = (#clients > 0 and clients[1].config.root_dir)
    or vim.fs.root(vim.api.nvim_buf_get_name(0), { "compile_commands.json", ".clangd", "compile_flags.txt" })
    or PROJECT_DIR
  local db_path = root .. "/compile_commands.json"
  vim.notify(string.format("clangd root: %s\nDB present: %s\nDB path: %s",
    root,
    db_present_at(db_path) and "1" or "0",
    db_path
  ), vim.log.levels.INFO, { title = "ELRS clangd" })
end

-- === Make :make / :make! work ===
-- Errors will go to quickfix (gcc-style output from PIO is compatible).
-- Use :cnext / :cprev / :copen after :make
vim.opt_local.makeprg = project_cmd(build_cmd(NIMBUS_ENV))

-- Optional: make the quickfix a bit nicer for this project
vim.opt_local.errorformat:append("%f:%l:%c: %t%*[^:]: %m") -- fallback

-- === User commands (run with :ELRSBuild etc.) ===
vim.api.nvim_buf_create_user_command(0, "ELRSBuild", function()
  floatterm.run(project_cmd(build_cmd(NIMBUS_ENV)))
end, { desc = "Build the Nimbus TX target (PIO)" })

vim.api.nvim_buf_create_user_command(0, "ELRSBuildNative", function()
  floatterm.run(project_cmd(build_cmd(NATIVE_ENV)))
end, { desc = "Build native env (for tests)" })

vim.api.nvim_buf_create_user_command(0, "ELRSTest", function()
  floatterm.run(project_cmd(pio_prefix() .. " pio test -e " .. NATIVE_ENV))
end, { desc = "Run pio test -e native (as in CI)" })

vim.api.nvim_buf_create_user_command(0, "ELRSCompDB", function(opts)
  local env = (opts.args and opts.args ~= "") and opts.args or NIMBUS_ENV
  local cmd
  if env == NIMBUS_ENV then
    cmd = "make compdb"
  elseif env == NATIVE_ENV then
    cmd = "make compdb-native"
  else
    cmd = "if [ -L compile_commands.json ]; then rm -f compile_commands.json; fi && " .. build_cmd(env, "compiledb")
  end
  floatterm.run(project_cmd(cmd))
end, { nargs = "?", desc = "Generate compile_commands.json for an env (default: nimbus)" })

vim.api.nvim_buf_create_user_command(0, "ELRSCompDBLink", function(opts)
  if opts.args and opts.args ~= "" then
    vim.notify(":ELRSCompDBLink no longer switches symlinks; run :ELRSCompDB " .. opts.args .. " first",
      vim.log.levels.WARN)
  end

  local db_path = project_file("compile_commands.json")
  if not db_present_at(db_path) then
    vim.notify("Missing " .. db_path .. "; run :ELRSCompDB or `make compdb` first", vim.log.levels.ERROR)
    return
  end

  restart_clangd()
end, { nargs = "?", desc = "Validate compile DB + restart clangd" })

vim.api.nvim_buf_create_user_command(0, "ELRSClangdStatus", function()
  clangd_status()
end, { desc = "Show clangd root and compile DB status" })

vim.api.nvim_buf_create_user_command(0, "ELRSValidate", function()
  -- One-shot: build + regenerate DB. Great before/after change check.
  floatterm.run(project_cmd("make validate"))
  -- After the terminal finishes you may still want :ELRSRestartClangd
end, { desc = "Build + update compile DB (full validation)" })

-- === Buffer-local keymaps (override global CMake ones only inside this tree) ===
local function bufmap(mode, lhs, rhs, desc)
  vim.keymap.set(mode, lhs, rhs, { buffer = true, silent = true, desc = desc })
end

bufmap("n", "<leader>cb", "<cmd>ELRSBuild<cr>", "ELRS: Build Nimbus target")
bufmap("n", "<leader>cB", "<cmd>ELRSBuildNative<cr>", "ELRS: Build native")
bufmap("n", "<leader>ct", "<cmd>ELRSTest<cr>", "ELRS: pio test -e native")
bufmap("n", "<leader>cc", "<cmd>ELRSCompDB<cr>", "ELRS: Generate compile DB (nimbus)")
bufmap("n", "<leader>cC", "<cmd>ELRSCompDBLink<cr>", "ELRS: Check compile DB + restart clangd")
bufmap("n", "<leader>cV", "<cmd>ELRSValidate<cr>", "ELRS: Validate (build + compdb)")

-- Explicitly register with which-key so they appear in the popup under <leader>c (Code)
-- even though they are buffer-local.
if wk_ok then
  wk.add({
    { "<leader>c",  group = "Code", buffer = 0 },  -- reinforce / merge with global
    { "<leader>cb", desc = "Build Nimbus target", buffer = 0 },
    { "<leader>cB", desc = "Build native", buffer = 0 },
    { "<leader>ct", desc = "pio test -e native", buffer = 0 },
    { "<leader>cc", desc = "Generate compile DB (nimbus)", buffer = 0 },
    { "<leader>cC", desc = "Check compile DB + restart clangd", buffer = 0 },
    { "<leader>cV", desc = "Validate (build + compdb)", buffer = 0 },
  })
end

-- Quick help
vim.api.nvim_buf_create_user_command(0, "ELRSHelp", function()
  vim.notify([[
ELRS (Nimbus) Neovim helpers:
  :make                 → build Nimbus (errors → quickfix)
  <leader>cb            → build in floating terminal
  <leader>ct            → run native tests
  <leader>cc            → generate compile DB for clangd
  <leader>cC            → check compile DB + restart clangd
  <leader>cV            → full build + DB update (great before/after your edits)
  :ELRSValidate         → full build + DB update (recommended pre/post change)
  :ELRSClangdStatus     → show clangd root + DB status

Regenerate DB after code changes so clangd sees the new -D/-I flags.
Current regulatory: ]] .. REGULATORY .. " / env: " .. NIMBUS_ENV, vim.log.levels.INFO)
end, { desc = "Show ELRS project keybindings" })

-- Friendly message + LSP sanity on load
local loaded_key = "elrs_nvim_loaded_" .. tostring(vim.api.nvim_get_current_buf())
if vim.b[loaded_key] ~= true then
  vim.b[loaded_key] = true
  vim.defer_fn(function()
    if not vim.api.nvim_buf_is_valid(0) then return end
    vim.notify("ELRS project loaded — <leader>cb to build, :ELRSHelp for more", vim.log.levels.INFO, { title = "ExpressLRS" })

    -- Quick sanity for clangd (helps when you are lost with include errors)
    vim.defer_fn(function()
      local clients = vim.lsp.get_clients({ bufnr = 0, name = "clangd" })
      if #clients > 0 then
        local root = clients[1].config.root_dir or "(unknown)"
        local db_path = root ~= "(unknown)" and (root .. "/compile_commands.json") or project_file("compile_commands.json")
        local has_db = db_present_at(db_path)
        vim.notify(string.format("clangd root: %s  (DB present: %s)", root, has_db and "1" or "0"),
          vim.log.levels.INFO, { title = "ELRS clangd" })
      end
    end, 800)
  end, 300)
end

-- Easy restart for clangd (since you don't have the classic :LspRestart from lspconfig)
vim.api.nvim_buf_create_user_command(0, "ELRSRestartClangd", function()
  restart_clangd()
end, { desc = "Restart clangd client (for when DB or config changes)" })

-- Also provide a global version if you want it outside the project
if not vim.g.elrs_clangd_restart_defined then
  vim.g.elrs_clangd_restart_defined = true
  vim.api.nvim_create_user_command("LspRestartClangd", function()
    restart_clangd()
  end, { desc = "Restart only the clangd client" })
end
