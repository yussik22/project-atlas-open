import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, realpathSync, rmSync, writeFileSync } from 'node:fs';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative, resolve } from 'node:path';

import { Client } from '@modelcontextprotocol/client';
import { StdioClientTransport } from '@modelcontextprotocol/client/stdio';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const serverFile = resolve(root, 'tools/atlas-mcp.mjs');
const cli = resolve(process.env.ATLAS_CLI ?? join(root, 'build-clang', 'project_atlas.exe'));
const gui = resolve(process.env.ATLAS_GUI ?? join(dirname(cli), 'project_atlas_gui.exe'));
const bridgeEnv = {
  ...process.env, ATLAS_CLI: cli, ATLAS_WORKSPACE_ROOT: root, ATLAS_COMPILE_DB: '',
};

test('MCP tools expose Atlas map, find, interactions, facts, trace and runtime graph',
  { timeout: 480_000 }, async () => {
  const client = new Client({ name: 'atlas-integration-test', version: '0.1.0' });
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [serverFile],
    cwd: root,
    env: bridgeEnv,
  });
  await client.connect(transport);
  try {
    const { tools } = await client.listTools();
    assert.deepEqual(tools.map(tool => tool.name).sort(),
      ['atlas_compare_facts', 'atlas_compare_trace', 'atlas_connection_status', 'atlas_context',
        'atlas_debug_breakpoint', 'atlas_debug_continue', 'atlas_debug_launch',
        'atlas_debug_status', 'atlas_debug_step', 'atlas_debug_stop',
        'atlas_file_relations', 'atlas_find', 'atlas_interactions', 'atlas_project_map',
        'atlas_runtime_map', 'atlas_source', 'atlas_ui_compare_facts',
        'atlas_ui_focus_file', 'atlas_ui_focus_symbol', 'atlas_ui_generate_cards',
        'atlas_ui_list', 'atlas_ui_load_trace', 'atlas_ui_record_run', 'atlas_ui_scan',
        'atlas_ui_select_tab', 'atlas_ui_set_step', 'atlas_ui_show', 'atlas_ui_status',
        'atlas_ui_uml_focus', 'atlas_ui_uml_overview']);

    const connected = await client.callTool({ name: 'atlas_connection_status', arguments: {} });
    assert.equal(connected.isError, undefined);
    const connection = connected.structuredContent ?? JSON.parse(connected.content[0].text);
    assert.equal(connection.workspace_root, realpathSync(root));
    assert.equal(connection.cli, cli);

    const mapped = await client.callTool({ name: 'atlas_project_map', arguments: {} },
      { timeout: 120_000 });
    assert.equal(mapped.isError, undefined);
    const map = mapped.structuredContent ?? JSON.parse(mapped.content[0].text);
    assert.equal(map.operation, 'map');
    assert.ok(map.summary.file_count > 0);
    assert.equal(map.returned_files, map.files.length);

    const found = await client.callTool({
      name: 'atlas_find', arguments: { query: 'atlas::Symbol' },
    }, { timeout: 120_000 });
    assert.equal(found.isError, undefined);
    const payload = found.structuredContent ?? JSON.parse(found.content[0].text);
    assert.equal(payload.schema, 'project-atlas.agent.v1');
    assert.ok(payload.matched_files >= 1);
    assert.ok(payload.files.some(file => file.symbols.some(symbol =>
      symbol.qualified_name === 'atlas::Symbol' &&
      symbol.members.some(member => member.name === 'kind'))));

    const sliced = await client.callTool({
      name: 'atlas_source', arguments: { file: 'include/atlas/atlas.hpp', start: 1, count: 3 },
    });
    assert.equal(sliced.isError, undefined);
    assert.equal(sliced.structuredContent, undefined);
    const source = JSON.parse(sliced.content[0].text);
    assert.equal(source.schema, 'project-atlas.source.v1');
    assert.equal(source.returned_lines, 3);
    assert.equal(source.lines[0].text, '#pragma once');
    const staleSource = await client.callTool({
      name: 'atlas_source', arguments: {
        file: 'include/atlas/atlas.hpp', start: 1, count: 1,
        expected_hash: '0000000000000000',
      },
    });
    assert.equal(staleSource.isError, true);
    const escapedSource = await client.callTool({
      name: 'atlas_source', arguments: { file: '../outside.cpp', start: 1, count: 1 },
    });
    assert.equal(escapedSource.isError, true);

    const related = await client.callTool({
      name: 'atlas_file_relations', arguments: { file: 'include/atlas/atlas.hpp' },
    }, { timeout: 120_000 });
    assert.equal(related.isError, undefined);
    assert.equal(related.structuredContent, undefined);
    const relations = JSON.parse(related.content[0].text);
    assert.equal(relations.schema, 'project-atlas.file-relations.v1');
    assert.equal(relations.path, 'include/atlas/atlas.hpp');
    assert.equal(relations.returned_edges, relations.edges.length);

    const packed = await client.callTool({
      name: 'atlas_context', arguments: { task: 'scan_project', budget: 400 },
    }, { timeout: 120_000 });
    assert.equal(packed.isError, undefined);
    assert.equal(packed.structuredContent, undefined);
    const context = packed.structuredContent ?? JSON.parse(packed.content[0].text);
    assert.equal(context.schema, 'project-atlas.context.v1');
    assert.ok(context.matched_files >= 1);
    assert.ok(context.files.some(file => file.path === 'src/scan.cpp'));
    assert.ok(JSON.stringify(context).length <= 1600);

    const integrated = await client.callTool({
      name: 'atlas_interactions',
      arguments: { trace: 'examples/runtime_memory.trace', step: 5 },
    }, { timeout: 120_000 });
    assert.equal(integrated.isError, undefined);
    const evidence = integrated.structuredContent ?? JSON.parse(integrated.content[0].text);
    assert.equal(evidence.schema, 'project-atlas.interactions.v1');
    assert.equal(evidence.runtime_step, 5);
    assert.ok(evidence.edges.some(edge => edge.kind === 'static_call'));
    assert.equal(evidence.edges.filter(edge => edge.kind === 'observed_link').length, 2);
    assert.ok(evidence.nodes.some(node => node.id === 'runtime:heap.buffer'));

    const compared = await client.callTool({
      name: 'atlas_compare_facts',
      arguments: {
        expected: 'examples/memory_expected.facts',
        actual: 'examples/memory_actual.facts',
      },
    });
    assert.equal(compared.isError, undefined);
    const differences = compared.structuredContent ?? JSON.parse(compared.content[0].text);
    assert.equal(differences.all_match, false);
    assert.ok(differences.entries.some(entry => entry.kind === 'mismatch'));

    const traced = await client.callTool({
      name: 'atlas_compare_trace',
      arguments: {
        expected: 'examples/memory_expected.trace',
        actual: 'examples/memory_actual.trace',
      },
    });
    assert.equal(traced.isError, undefined);
    const timeline = traced.structuredContent ?? JSON.parse(traced.content[0].text);
    assert.equal(timeline.all_match, false);
    assert.equal(timeline.first_difference.step, 2);
    assert.equal(timeline.first_difference.actual.value, 'Cache');

    const graphed = await client.callTool({
      name: 'atlas_runtime_map',
      arguments: { trace: 'examples/runtime_memory.trace', step: 5 },
    });
    assert.equal(graphed.isError, undefined);
    const graph = graphed.structuredContent ?? JSON.parse(graphed.content[0].text);
    assert.equal(graph.operation, 'runtime');
    assert.equal(graph.nodes.length, 3);
    assert.deepEqual(graph.links.map(link => link.relation).sort(),
      ['owns', 'points_to']);

    const refused = await client.callTool({
      name: 'atlas_compare_facts',
      arguments: { expected: '../outside.facts', actual: 'examples/memory_actual.facts' },
    });
    assert.equal(refused.isError, true);
  } finally {
    await client.close();
  }
});

test('MCP controls a separate configured C++ workspace, including the GUI',
  { timeout: 120_000, skip: process.platform !== 'win32' }, async () => {
    const fixtureBase = join(root, 'atlas-test-workspace');
    mkdirSync(fixtureBase, { recursive: true });
    const fixtureRoot = realpathSync(fixtureBase);
    const relativeRoot = relative(realpathSync(root), fixtureRoot);
    assert.ok(relativeRoot && relativeRoot !== '..' && !relativeRoot.startsWith('..\\'));
    const workspace = mkdtempSync(join(fixtureBase, 'mcp-external-'));
    const sourceFile = join(workspace, 'demo.cpp');
    writeFileSync(sourceFile, 'struct ExternalWidget { int value() const { return 7; } };\nint main() { ExternalWidget widget; return widget.value(); }\n');
    writeFileSync(join(workspace, 'compile_commands.json'), JSON.stringify([{
      directory: workspace, file: sourceFile,
      arguments: ['clang++', '-std=c++23', '-c', sourceFile],
    }]));
    const child = spawn(gui, [], { cwd: root, windowsHide: true, stdio: 'ignore' });
    const client = new Client({ name: 'atlas-external-workspace-test', version: '0.1.0' });
    const transport = new StdioClientTransport({
      command: process.execPath, args: [serverFile], cwd: root,
      env: { ...bridgeEnv, ATLAS_WORKSPACE_ROOT: workspace },
    });
    const call = async (name, args = {}) => {
      const result = await client.callTool({ name, arguments: args }, { timeout: 60_000 });
      return { result, data: result.structuredContent ?? JSON.parse(result.content[0].text) };
    };
    try {
      await client.connect(transport);
      const connection = await call('atlas_connection_status');
      assert.equal(connection.result.isError, undefined);
      assert.equal(connection.data.workspace_root, realpathSync(workspace));
      assert.equal(connection.data.compilation_database, realpathSync(workspace));
      const mapped = await call('atlas_project_map');
      assert.equal(mapped.result.isError, undefined);
      assert.equal(mapped.data.root, realpathSync(workspace).replaceAll('\\', '/'));
      assert.equal(mapped.data.summary.file_count, 1);
      const found = await call('atlas_find', { query: 'ExternalWidget' });
      assert.equal(found.result.isError, undefined);
      assert.ok(found.data.matched_files > 0);
      const source = await call('atlas_source', { file: 'demo.cpp', start: 1, count: 1 });
      assert.equal(source.result.isError, undefined);
      assert.match(source.data.lines[0].text, /ExternalWidget/);
      const escaped = await client.callTool({
        name: 'atlas_source',
        arguments: { file: '../src/scan.cpp', start: 1, count: 1 },
      });
      assert.equal(escaped.isError, true);
      const until = Date.now() + 20_000;
      let status;
      do {
        status = await call('atlas_ui_status', { pid: child.pid });
        if (!status.result.isError && status.data.ok) break;
        await new Promise(done => setTimeout(done, 150));
      } while (Date.now() < until);
      assert.equal(status.result.isError, undefined);
      const scanning = await call('atlas_ui_scan', { pid: child.pid });
      assert.equal(scanning.result.isError, undefined);
      const scanDeadline = Date.now() + 60_000;
      do {
        status = await call('atlas_ui_status', { pid: child.pid });
        if (!status.result.isError && !status.data.scanning &&
            status.data.project_root === workspace) break;
        await new Promise(done => setTimeout(done, 150));
      } while (Date.now() < scanDeadline);
      assert.equal(status.data.project_root, workspace);
      assert.equal(status.data.scanning, false);
      const focused = await call('atlas_ui_focus_file', { file: 'demo.cpp', pid: child.pid });
      assert.equal(focused.result.isError, undefined);
      assert.equal(focused.data.selected_file, 'demo.cpp');
      const otherClient = new Client({ name: 'atlas-mismatched-workspace-test', version: '0.1.0' });
      const otherTransport = new StdioClientTransport({
        command: process.execPath, args: [serverFile], cwd: root, env: bridgeEnv,
      });
      try {
        await otherClient.connect(otherTransport);
        const refused = await otherClient.callTool({
          name: 'atlas_ui_generate_cards', arguments: { pid: child.pid },
        });
        assert.equal(refused.isError, true);
        assert.match((refused.structuredContent ?? JSON.parse(refused.content[0].text)).error,
          /different project/);
      } finally {
        await otherClient.close();
      }
    } finally {
      child.kill();
      await client.close();
      const resolvedWorkspace = realpathSync(workspace);
      const relativeFixture = relative(fixtureRoot, resolvedWorkspace);
      if (relativeFixture && relativeFixture !== '..' &&
          !relativeFixture.startsWith('..\\')) {
        rmSync(resolvedWorkspace, { recursive: true, force: true });
      }
    }
  });

test('MCP controls the live native GUI and embedded debugger without UI clicks',
  { timeout: 300_000, skip: process.platform !== 'win32' }, async () => {
    assert.ok(existsSync(gui), `GUI executable missing: ${gui}`);
    const child = spawn(gui, [], { cwd: root, windowsHide: true, stdio: 'ignore' });
    const client = new Client({ name: 'atlas-live-control-test', version: '0.1.0' });
    const transport = new StdioClientTransport({
      command: process.execPath, args: [serverFile], cwd: root, env: bridgeEnv,
    });
    await client.connect(transport);
    const call = async (name, args = {}) => {
      const result = await client.callTool({ name, arguments: args });
      return { result, data: result.structuredContent ?? JSON.parse(result.content[0].text) };
    };
    const waitFor = async (predicate, limitMs = 20_000) => {
      const until = Date.now() + limitMs;
      while (Date.now() < until) {
        const current = await call('atlas_ui_status', { pid: child.pid });
        if (!current.result.isError && predicate(current.data)) return current.data;
        await new Promise(done => setTimeout(done, 150));
      }
      throw new Error('Atlas GUI did not reach the expected state');
    };
    try {
      await waitFor(data => data.ok);
      const shown = await call('atlas_ui_show', { pid: child.pid });
      assert.equal(shown.result.isError, undefined);
      assert.equal(shown.data.window_visible, true);
      const prematureCards = await call('atlas_ui_generate_cards', { pid: child.pid });
      assert.equal(prematureCards.result.isError, true);
      const debugLaunch = await call('atlas_debug_launch', {
        executable: `${relative(root, dirname(cli)).replaceAll('\\', '/')}/project_atlas_debug_demo.exe`, pid: child.pid,
      });
      assert.equal(debugLaunch.result.isError, undefined);
      assert.equal(debugLaunch.data.active_tab, 5);
      await waitFor(data => data.debug?.phase === 'paused' && data.debug.reason === 'entry');
      const breakpoint = await call('atlas_debug_breakpoint', {
        file: 'examples/debugger_demo.cpp', line: 3, pid: child.pid,
      });
      assert.equal(breakpoint.result.isError, undefined);
      await waitFor(data => data.debug?.breakpoints?.[0]?.verified === true);
      const continued = await call('atlas_debug_continue', { pid: child.pid });
      assert.equal(continued.result.isError, undefined);
      const atBreakpoint = await waitFor(data => data.debug?.reason === 'breakpoint' &&
        data.debug.frames?.[0]?.line === 3);
      assert.equal(atBreakpoint.debug.variables.find(item => item.name === 'counter')?.value, '7');
      const steppedDebug = await call('atlas_debug_step', { pid: child.pid });
      assert.equal(steppedDebug.result.isError, undefined);
      await waitFor(data => data.debug?.reason === 'step' &&
        data.debug.frames?.[0]?.line === 4);
      const stoppedDebug = await call('atlas_debug_stop', { pid: child.pid });
      assert.equal(stoppedDebug.result.isError, undefined);
      await waitFor(data => data.debug?.phase === 'stopped');
      const loaded = await call('atlas_ui_load_trace', {
        trace: 'examples/runtime_memory.trace', pid: child.pid,
      });
      assert.equal(loaded.result.isError, undefined);
      assert.equal(loaded.data.active_tab, 4);
      assert.equal(loaded.data.trace_loaded, true);
      const stepped = await call('atlas_ui_set_step', { step: 5, pid: child.pid });
      assert.equal(stepped.result.isError, undefined);
      assert.equal(stepped.data.step, 5);
      assert.equal(stepped.data.node_count, 3);
      assert.equal(stepped.data.link_count, 2);
      const invalid = await call('atlas_ui_set_step', { step: 999, pid: child.pid });
      assert.equal(invalid.result.isError, true);
      assert.equal(invalid.data.step, 5);
      const tabbed = await call('atlas_ui_select_tab', { tab: 'uml', pid: child.pid });
      assert.equal(tabbed.data.active_tab, 3);
      const prematureUmlFocus = await call('atlas_ui_uml_focus', { pid: child.pid });
      assert.equal(prematureUmlFocus.result.isError, true);
      const scanning = await call('atlas_ui_scan', { pid: child.pid });
      assert.equal(scanning.result.isError, undefined);
      await waitFor(data => !data.scanning && data.project_root === root, 120_000);
      const recorded = await call('atlas_ui_record_run', {
        executable: `${relative(root, dirname(cli)).replaceAll('\\', '/')}/project_atlas_auto_demo.exe`, pid: child.pid,
      });
      assert.equal(recorded.result.isError, undefined);
      const afterRecord = await waitFor(data => !data.recording &&
        data.trace_path.includes('.project-atlas') && data.active_tab === 4);
      // The eleventh event is the terminal capture.complete record.
      assert.equal(afterRecord.total_steps, 11);
      const umlOverview = await call('atlas_ui_uml_overview', { pid: child.pid });
      assert.equal(umlOverview.result.isError, undefined);
      assert.equal(umlOverview.data.uml_overview, true);
      assert.ok(umlOverview.data.uml_class_count > 0);
      const focusedFile = await call('atlas_ui_focus_file', {
        file: 'include/atlas/atlas.hpp', pid: child.pid,
      });
      assert.equal(focusedFile.result.isError, undefined);
      assert.equal(focusedFile.data.active_tab, 1);
      assert.equal(focusedFile.data.selected_file, 'include\\atlas\\atlas.hpp');
      const focusedSymbol = await call('atlas_ui_focus_symbol', {
        symbol: 'atlas::Symbol', pid: child.pid,
      });
      assert.equal(focusedSymbol.result.isError, undefined);
      assert.equal(focusedSymbol.data.selected_symbol, 'atlas::Symbol');
      const interactionsTab = await call('atlas_ui_select_tab', {
        tab: 'interactions', pid: child.pid,
      });
      assert.equal(interactionsTab.result.isError, undefined);
      assert.equal(interactionsTab.data.active_tab, 6);
      const umlFocus = await call('atlas_ui_uml_focus', { pid: child.pid });
      assert.equal(umlFocus.result.isError, undefined);
      assert.equal(umlFocus.data.uml_overview, false);
      assert.equal(umlFocus.data.active_tab, 3);
      const compared = await call('atlas_ui_compare_facts', {
        expected: 'examples/memory_expected.facts',
        actual: 'examples/memory_actual.facts', pid: child.pid,
      });
      assert.equal(compared.result.isError, undefined);
      assert.equal(compared.data.active_tab, 2);
      assert.match(compared.data.status, /differences/);
    } finally {
      child.kill();
      await client.close();
    }
  });
