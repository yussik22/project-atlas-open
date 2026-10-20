import { spawn } from 'node:child_process';
import { existsSync, realpathSync, statSync } from 'node:fs';
import { basename, dirname, extname, isAbsolute, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

import { McpServer } from '@modelcontextprotocol/server';
import { serveStdio } from '@modelcontextprotocol/server/stdio';
import * as z from 'zod/v4';

const installationRoot = realpathSync(resolve(dirname(fileURLToPath(import.meta.url)), '..'));
const buildDirectories = ['build-fixed', 'build-clang', 'build-ninja', 'build'];

function workspaceRoot() {
  const configured = process.env.ATLAS_WORKSPACE_ROOT;
  const root = realpathSync(configured ? resolve(configured) : installationRoot);
  if (!statSync(root).isDirectory()) throw new Error('Atlas workspace is not a directory');
  return root;
}

function findExecutable() {
  if (process.env.ATLAS_CLI) {
    const candidate = resolve(process.env.ATLAS_CLI);
    if (!existsSync(candidate) || !statSync(candidate).isFile()) {
      throw new Error(`ATLAS_CLI is not a file: ${candidate}`);
    }
    return candidate;
  }
  for (const build of buildDirectories) {
    const candidate = join(installationRoot, build, process.platform === 'win32'
      ? 'project_atlas.exe' : 'project_atlas');
    if (existsSync(candidate)) return candidate;
  }
  throw new Error('Project Atlas CLI is not built; build the CMake project first.');
}

function compileDatabaseFor(executable) {
  const root = workspaceRoot();
  if (process.env.ATLAS_COMPILE_DB) {
    const configured = resolve(root, process.env.ATLAS_COMPILE_DB);
    const directory = basename(configured) === 'compile_commands.json'
      ? dirname(configured) : configured;
    const database = join(directory, 'compile_commands.json');
    if (!existsSync(database) || !statSync(database).isFile()) {
      throw new Error(`ATLAS_COMPILE_DB has no compile_commands.json: ${directory}`);
    }
    return directory;
  }
  const candidates = root === installationRoot
    ? [dirname(executable), root, ...buildDirectories.map(build => join(root, build))]
    : [root, ...buildDirectories.map(build => join(root, build))];
  return candidates.find(directory => {
    const database = join(directory, 'compile_commands.json');
    return existsSync(database) && statSync(database).isFile();
  }) ?? null;
}

function runAtlas(arguments_) {
  const executable = findExecutable();
  const args = [...arguments_];
  if (arguments_[0] === 'map' || arguments_[0] === 'find') {
    const database = compileDatabaseFor(executable);
    if (database) args.push(database);
  }
  return new Promise((resolveResult, reject) => {
    const child = spawn(executable, ['agent', ...args], {
      cwd: installationRoot,
      windowsHide: true,
      shell: false,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    let stdoutBytes = 0;
    let settled = false;
    const timer = setTimeout(() => {
      fail(new Error('Atlas command exceeded 120 seconds'));
    }, 120_000);
    function fail(error) {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      child.kill();
      reject(error);
    }
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', chunk => {
      if (settled) return;
      stdoutBytes += Buffer.byteLength(chunk, 'utf8');
      if (stdoutBytes > 16 * 1024 * 1024) {
        fail(new Error('Atlas response exceeds 16 MiB'));
        return;
      }
      stdout += chunk;
    });
    child.stderr.on('data', chunk => {
      if (settled) return;
      stderr += chunk;
      if (Buffer.byteLength(stderr, 'utf8') > 64 * 1024) {
        fail(new Error('Atlas stderr exceeds 64 KiB'));
      }
    });
    child.on('error', fail);
    child.on('close', code => {
      clearTimeout(timer);
      if (settled) return;
      try {
        const payload = JSON.parse(stdout);
        if (code !== 0 && code !== 2) {
          reject(new Error(payload.error ?? stderr.trim() ?? `Atlas exited ${code}`));
        } else {
          resolveResult(payload);
        }
      } catch (error) {
        reject(new Error(`Atlas returned invalid JSON: ${error.message}; ${stderr.trim()}`));
      }
    });
  });
}

function projectFilePath(value, extension) {
  if (isAbsolute(value)) throw new Error('Paths must be relative to the Atlas workspace');
  const root = workspaceRoot();
  const unresolved = resolve(root, value);
  const lexical = relative(root, unresolved);
  if (!lexical || lexical === '..' || lexical.startsWith(`..${sep}`) || isAbsolute(lexical)) {
    throw new Error('Path must refer to a file inside the Atlas workspace');
  }
  const candidate = realpathSync(unresolved);
  const inside = relative(root, candidate);
  if (!inside || inside === '..' || inside.startsWith(`..${sep}`) || isAbsolute(inside)) {
    throw new Error('Path must refer to a file inside the Atlas workspace');
  }
  if (!statSync(candidate).isFile()) throw new Error('Expected a file inside the Atlas workspace');
  if (extension && extname(candidate) !== extension) {
    throw new Error(`Expected a ${extension} file inside the Atlas project`);
  }
  return candidate;
}

function sourceFilePath(value) {
  const candidate = projectFilePath(value, null);
  if (!['.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.hxx', '.inl', '.tpp', '.ixx']
    .includes(extname(candidate))) throw new Error('Expected a C/C++ source file');
  return candidate;
}

function resultOf(payload) {
  return {
    content: [{ type: 'text', text: JSON.stringify(payload) }],
    structuredContent: payload,
  };
}

function uiResultOf(payload) {
  const result = resultOf(payload);
  if (payload.ok === false) result.isError = true;
  return result;
}

function uiErrorOf(error) {
  return uiResultOf({
    schema: 'project-atlas.ui.v1', operation: 'ui', ok: false,
    error: String(error.message ?? error),
  });
}

function errorOf(error) {
  return { content: [{ type: 'text', text: String(error.message ?? error) }], isError: true };
}

async function requireGuiWorkspace(pid) {
  const state = await runAtlas(['ui', 'status', ...(pid ? [String(pid)] : [])]);
  if (!state.ok) throw new Error(state.error || 'Atlas GUI is unavailable');
  if (!state.project_root) throw new Error('Scan the configured workspace in Atlas first');
  const actual = realpathSync(state.project_root);
  const expected = workspaceRoot();
  const equal = process.platform === 'win32'
    ? actual.toLowerCase() === expected.toLowerCase() : actual === expected;
  if (!equal) {
    throw new Error(`Atlas GUI shows a different project (${actual}); scan ${expected} first`);
  }
}

function createServer() {
  const server = new McpServer({ name: 'project-atlas', version: '0.11.0' });
  server.registerTool('atlas_connection_status', {
    description: 'Show the configured workspace, Atlas CLI and compilation database used by this bridge. No files are changed.',
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  }, async () => {
    try {
      const executable = findExecutable();
      return resultOf({
        schema: 'project-atlas.connection.v1',
        installation_root: installationRoot,
        workspace_root: workspaceRoot(),
        cli: executable,
        compilation_database: compileDatabaseFor(executable),
      });
    } catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_project_map', {
    description: 'Read the configured C++ workspace AST map as JSON (up to 200 files). No files are changed.',
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  }, async () => {
    try { return resultOf(await runAtlas(['map', workspaceRoot()])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_find', {
    description: 'Find C++ files or symbols by path/name/USR substring, including line, Clang USR, bases and diagnostics. No files are changed.',
    inputSchema: z.object({ query: z.string().trim().min(1).max(200) }),
    annotations: { readOnlyHint: true },
  }, async ({ query }) => {
    try { return resultOf(await runAtlas(['find', workspaceRoot(), query])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_context', {
    description: 'Return a compact, lexically ranked C++ evidence packet for a task: paths, matching symbols/calls, source lines and hash-verified snippets. The token budget is approximate; response bytes are capped. No files are changed.',
    inputSchema: z.object({
      task: z.string().trim().min(1).max(200),
      budget: z.number().int().min(400).max(4000).default(1500),
    }),
    annotations: { readOnlyHint: true },
  }, async ({ task, budget }) => {
    try {
      const args = ['context', workspaceRoot(), '--task', task, '--budget', String(budget)];
      const database = compileDatabaseFor(findExecutable());
      if (database) args.push('--compile-db', database);
      return { content: [{ type: 'text', text: JSON.stringify(await runAtlas(args)) }] };
    } catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_source', {
    description: 'Read an exact, bounded 1..80-line UTF-8 C/C++ source slice with line numbers and FNV-1a hash. Optional expected_hash rejects stale context. Fast: no AST scan. No files are changed.',
    inputSchema: z.object({
      file: z.string().min(1),
      start: z.number().int().min(1),
      count: z.number().int().min(1).max(80).default(30),
      expected_hash: z.string().regex(/^[0-9a-f]{16}$/).optional(),
    }),
    annotations: { readOnlyHint: true },
  }, async ({ file, start, count, expected_hash }) => {
    try {
      sourceFilePath(file);
      const args = ['source', workspaceRoot(), file, String(start), String(count)];
      if (expected_hash) args.push(expected_hash);
      return { content: [{ type: 'text', text: JSON.stringify(await runAtlas(args)) }] };
    } catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_file_relations', {
    description: 'Read only direct AST call, inheritance, and field-type relationships entering or leaving one scanned C/C++ file. Shows evidence and truncation; not complete runtime impact. No files are changed.',
    inputSchema: z.object({ file: z.string().min(1) }),
    annotations: { readOnlyHint: true },
  }, async ({ file }) => {
    try {
      sourceFilePath(file);
      const args = ['file-relations', workspaceRoot(), file];
      const database = compileDatabaseFor(findExecutable());
      if (database) args.push(database);
      return { content: [{ type: 'text', text: JSON.stringify(await runAtlas(args)) }] };
    } catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_interactions', {
    description: 'Read a provenance-labelled interaction map: AST inheritance/field/call evidence plus optional observed runtime links. Runtime IDs are never guessed to be source symbols. No files are changed.',
    inputSchema: z.object({
      trace: z.string().min(1).optional(),
      step: z.number().int().min(0).optional(),
    }),
    annotations: { readOnlyHint: true },
  }, async ({ trace, step }) => {
    try {
      if (step !== undefined && !trace) throw new Error('step requires trace');
      const args = ['interactions', workspaceRoot()];
      const database = compileDatabaseFor(findExecutable());
      if (database) args.push('--compile-db', database);
      if (trace) args.push('--trace', projectFilePath(trace, '.trace'));
      if (step !== undefined) args.push('--step', String(step));
      return resultOf(await runAtlas(args));
    } catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_compare_facts', {
    description: 'Compare two existing .facts files inside the configured workspace. No files are changed.',
    inputSchema: z.object({
      expected: z.string().min(1),
      actual: z.string().min(1),
    }),
    annotations: { readOnlyHint: true },
  }, async ({ expected, actual }) => {
    try { return resultOf(await runAtlas(['compare',
      projectFilePath(expected, '.facts'), projectFilePath(actual, '.facts')])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_compare_trace', {
    description: 'Compare two ordered .trace memory-lifecycle logs and report the first divergent step. No files are changed.',
    inputSchema: z.object({
      expected: z.string().min(1),
      actual: z.string().min(1),
    }),
    annotations: { readOnlyHint: true },
  }, async ({ expected, actual }) => {
    try { return resultOf(await runAtlas(['trace',
      projectFilePath(expected, '.trace'), projectFilePath(actual, '.trace')])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_runtime_map', {
    description: 'Replay an explicitly instrumented runtime .trace into variable/object nodes and links at a chosen step. No files are changed.',
    inputSchema: z.object({
      trace: z.string().min(1),
      step: z.number().int().min(0).optional(),
    }),
    annotations: { readOnlyHint: true },
  }, async ({ trace, step }) => {
    try {
      const args = ['runtime', projectFilePath(trace, '.trace')];
      if (step !== undefined) args.push(String(step));
      return resultOf(await runAtlas(args));
    } catch (error) { return errorOf(error); }
  });
  const pid = z.number().int().positive().optional();
  server.registerTool('atlas_ui_list', {
    description: 'List running native Project Atlas windows and their PIDs. No window is changed.',
    inputSchema: z.object({}),
    annotations: { readOnlyHint: true },
  }, async () => {
    try { return resultOf(await runAtlas(['ui', 'list'])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_status', {
    description: 'Read the live Atlas window state: project, selected tab, loaded trace, timeline step, graph counts, and busy flags.',
    inputSchema: z.object({ pid }),
    annotations: { readOnlyHint: true },
  }, async ({ pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'status', ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_show', {
    description: 'Show and raise an existing native Project Atlas window, including one launched hidden.',
    inputSchema: z.object({ pid }),
    annotations: { readOnlyHint: false },
  }, async ({ pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'show', ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  for (const [name, action, description] of [
    ['atlas_ui_uml_overview', 'uml-overview', 'Show the project-wide class map with inheritance and direct field-type links.'],
    ['atlas_ui_uml_focus', 'uml-focus', 'Show the selected class with its attributes, operations and base classes.'],
  ]) {
    server.registerTool(name, {
      description,
      inputSchema: z.object({ pid }),
      annotations: { readOnlyHint: false },
    }, async ({ pid: target }) => {
      try { return uiResultOf(await runAtlas(['ui', action,
        ...(target ? [String(target)] : [])])); }
      catch (error) { return errorOf(error); }
    });
  }
  server.registerTool('atlas_ui_load_trace', {
    description: 'Load an existing .trace inside the configured workspace into the running GUI and show Runtime map. Changes only the GUI view.',
    inputSchema: z.object({ trace: z.string().min(1), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ trace, pid: target }) => {
    try {
      return uiResultOf(await runAtlas(['ui', 'load-trace', projectFilePath(trace, '.trace'),
        ...(target ? [String(target)] : [])]));
    } catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_set_step', {
    description: 'Set the visible Runtime map timeline step in the running Atlas GUI; returns the resulting graph counts.',
    inputSchema: z.object({ step: z.number().int().min(0), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ step, pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'step', String(step),
      ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_select_tab', {
    description: 'Select an Atlas GUI tab: overview, source, expected_actual, uml, runtime_map, debugger, or interactions.',
    inputSchema: z.object({
      tab: z.enum(['overview', 'source', 'expected_actual', 'uml', 'runtime_map', 'debugger', 'interactions']), pid,
    }),
    annotations: { readOnlyHint: false },
  }, async ({ tab, pid: target }) => {
    try {
      const index = ['overview', 'source', 'expected_actual', 'uml', 'runtime_map', 'debugger', 'interactions'].indexOf(tab);
      return uiResultOf(await runAtlas(['ui', 'tab', String(index),
        ...(target ? [String(target)] : [])]));
    } catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_scan', {
    description: 'Start an asynchronous scan of the configured workspace in the GUI; poll atlas_ui_status for completion. Does not generate cards.',
    inputSchema: z.object({ pid }),
    annotations: { readOnlyHint: false },
  }, async ({ pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'scan', workspaceRoot(),
      ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_record_run', {
    description: 'Execute an instrumented .exe inside the configured workspace, record C++ allocations, and open the resulting trace in the GUI. This RUNS the selected executable; poll atlas_ui_status for completion.',
    inputSchema: z.object({ executable: z.string().min(1), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ executable, pid: target }) => {
    try {
      const program = projectFilePath(executable, '.exe');
      await requireGuiWorkspace(target);
      return uiResultOf(await runAtlas(['ui', 'record', program,
        ...(target ? [String(target)] : [])]));
    }
    catch (error) { return uiErrorOf(error); }
  });
  server.registerTool('atlas_ui_focus_file', {
    description: 'Select a scanned source file in the live Atlas GUI and show its source. The file must be inside the configured workspace.',
    inputSchema: z.object({ file: z.string().min(1), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ file, pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'focus-file', projectFilePath(file, null),
      ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_focus_symbol', {
    description: 'Select a symbol in the live Atlas GUI by exact qualified name or, preferably, its Clang USR.',
    inputSchema: z.object({ symbol: z.string().min(1).max(1000), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ symbol, pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'focus-symbol', symbol,
      ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_compare_facts', {
    description: 'Show the difference between two existing .facts files in the native Expected / actual tab. No files are changed.',
    inputSchema: z.object({ expected: z.string().min(1), actual: z.string().min(1), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ expected, actual, pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'compare-facts',
      projectFilePath(expected, '.facts'), projectFilePath(actual, '.facts'),
      ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_ui_generate_cards', {
    description: 'Explicitly generate .agent.md cards beside scanned source files and an index under .project-atlas/generated. Writes project files; preserves user-owned cards.',
    inputSchema: z.object({ pid }),
    annotations: { readOnlyHint: false },
  }, async ({ pid: target }) => {
    try {
      await requireGuiWorkspace(target);
      return uiResultOf(await runAtlas(['ui', 'generate-cards',
        ...(target ? [String(target)] : [])]));
    }
    catch (error) { return uiErrorOf(error); }
  });
  server.registerTool('atlas_debug_status', {
    description: 'Read the embedded Windows debugger state, current source location, call stack, local variables and breakpoint verification. Does not run the debuggee.',
    inputSchema: z.object({ pid }),
    annotations: { readOnlyHint: true },
  }, async ({ pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'status',
      ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_debug_launch', {
    description: 'Launch a project .exe under the embedded Windows debugger; it pauses at the initial loader breakpoint so source breakpoints can be set. This executes the selected program.',
    inputSchema: z.object({ executable: z.string().min(1), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ executable, pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'debug-launch',
      projectFilePath(executable, '.exe'), ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  server.registerTool('atlas_debug_breakpoint', {
    description: 'Set a source-line breakpoint in a paused debug session. Poll atlas_debug_status for verified and actual_line; PDB debug symbols are required.',
    inputSchema: z.object({ file: z.string().min(1), line: z.number().int().positive(), pid }),
    annotations: { readOnlyHint: false },
  }, async ({ file, line, pid: target }) => {
    try { return uiResultOf(await runAtlas(['ui', 'debug-break',
      projectFilePath(file, null), String(line), ...(target ? [String(target)] : [])])); }
    catch (error) { return errorOf(error); }
  });
  for (const [name, action, description] of [
    ['atlas_debug_continue', 'debug-continue', 'Resume a paused debuggee until the next breakpoint, exception, or exit.'],
    ['atlas_debug_step', 'debug-step', 'Single-step to the next source line in a paused debuggee (may enter called functions).'],
    ['atlas_debug_stop', 'debug-stop', 'Terminate the exact process launched by this Atlas debug session.'],
  ]) {
    server.registerTool(name, {
      description,
      inputSchema: z.object({ pid }),
      annotations: { readOnlyHint: false },
    }, async ({ pid: target }) => {
      try { return uiResultOf(await runAtlas(['ui', action,
        ...(target ? [String(target)] : [])])); }
      catch (error) { return errorOf(error); }
    });
  }
  return server;
}

void serveStdio(createServer);
