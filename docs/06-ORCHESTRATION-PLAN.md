To run an unattended, multi-week software factory inside a Proxmox LXC container building a hybrid **C++20 (Flecs/Jolt/Vulkan) \+ .NET 10 (Roslyn/Editor)** game engine powered by **Muse Spark 1.3 (muse-spark-1.3-contributor)** and **Unsloth embeddinggemma-2-GGUF**, every layer of your stack must be chosen based on how its actual source code behaves under autonomous load—not marketing claims.  
Below is the fact-based architectural breakdown of the harnesses, the 5 orchestrators you listed, the memory layer, the LSP/AST MCP stack, and the exact LXC deployment blueprint.

## **Part 1: The Harness Verdict (Hermes vs. Claude Code vs. Pi)**

We are driving muse-spark-1.3-contributor (1,048,576 token context window, exposed at \[https\://api.meta.ai/v1\](https\://api.meta.ai/v1) with both OpenAI and Anthropic SDK drop-in compatibility). How do these three harnesses actually handle LSP, AST, and multi-hour unattended loops?

| Feature / Mechanism | Pi (@mariozechner/pi-coding-agent) | Claude Code (@anthropic-ai/claude-code) | Hermes Agent (NousResearch/hermes-agent) |
| :---- | :---- | :---- | :---- |
| **LSP Integration** | **S-Tier (samfoy/pi-lsp-extension)**: Persistent background daemon (dist/lsp-daemon.js). **Auto-diagnostics hook**: Every time the agent runs write or edit, the extension intercepts the result and automatically appends up to 10 lines of compiler errors from the running LSP directly into the tool output. Zero wasted turns. | **B-Tier (Built-in since v2.0.74 / \$ENABLE\_LSP\_TOOL=1)**: Uses .lsp.json via claude-code-lsps marketplace. **Fatal Flaw**: Claude Code's hardcoded system prompt aggressively instructs the model to prefer Grep and Glob over LSP. In unattended runs, the model routinely ignores LSP tools unless forced via hooks. | **A-Tier (rewasa/hermes-code-intel-plugin)**: Python plugin (\~/.hermes/plugins/) providing 11 LSP tools (code\_definition, code\_callers, code\_impact, code\_diagnostics) with automatic fallback to Tree-sitter AST if a language server crashes. |
| **AST / Structural Refactor** | **Native in pi-lsp-extension**: Exposes ast\_search, code\_rewrite (dry-run by default), and code\_overview (Tree-sitter symbol map). | **None Native**: Requires external MCP (@notprolands/ast-grep-mcp or ast-grep/ast-grep-mcp) plus CLAUDE.md rules. | **Native Optional Skill \+ Plugin**: Ships with optional-skills/software-development/ast-grep/SKILL.md (with mechanical regex-validation helpers) \+ rewasa plugin's 8 Tree-sitter tools (code\_refactor, code\_capsule). |
| **Unattended Loop & Context** | **Deterministic & Hackable**: TypeScript core, zero bloat, pairs with clang.engineer/pi-workloop for bounded automatic continuation. | **Proprietary Heuristics**: Hardcoded Anthropic prompt-caching assumptions; when pointed at ANTHROPIC\_BASE\_URL=\[https\://api.meta.ai/v1\](https\://api.meta.ai/v1) for Muse Spark, system-prompt steering fights custom instructions. | **Built-in Learning Loop**: SQLite WAL \+ FTS5 session store, auto-compaction with Resolved/Pending state handoffs, ThreadPoolExecutor (8 parallel tool workers), and autonomous SKILL.md self-creation/curation (hermes curator). |
| **Subagent Spawning** | Handled via extensions or external orchestrator (openrig). | Native Task/Subagent tool, isolated context. | Built-in subagent delegation (max depth 2, 3 concurrent children, restricted toolsets). |

### **The Harness Decision: Dual-Harness (Pi for Workers \+ Hermes for Lead/Curator)**

> 1. **Use Pi (pi-coding-agent \+ samfoy/pi-lsp-extension) as your Primary Coding Worker Harness:** When writing C++20/Vulkan and C\#13 unsafe pointer code, **Pi's auto-diagnostics-on-edit** is unbeatable. The millisecond Pi writes a C++ or C\# file, clangd or csharp-ls validates it and injects any compiler error straight into the same tool turn. The agent fixes its own syntax/type errors before finishing its thought.  
> 2. **Use Hermes Agent (v0.14.0 \+ rewasa/hermes-code-intel-plugin) as your Architect / QA / Memory Curator:** Hermes has a native closed learning loop (MEMORY.md, hermes curator, and code\_impact blast-radius analysis). When a worker finishes a complex milestone, Hermes audits the diff, extracts reusable engine patterns into new SKILL.md files, and prunes stale skills.  
> 3. *(Optional)* Keep **Claude Code** in the container strictly as a fallback adapter for orchestrators that default to it, pointed at Muse Spark via ANTHROPIC\_BASE\_URL=\[https\://api.meta.ai/v1\](https\://api.meta.ai/v1).

## **Part 2: The Orchestrator Showdown (Hard Facts on the 5 Repos)**

You listed five specific orchestrators. You need **agents driving agents**, **Kanban visibility**, **isolated Git worktrees**, and **non-stop execution inside an LXC container** until all 24 GitHub issues across Milestones 1–5 are merged and verified.

| Orchestrator Repo | Stack & Architecture | Native Harnesses | How It Actually Works Under the Hood | Fatal Flaw for Unattended LXC |
| :---- | :---- | :---- | :---- | :---- |
| **1\. agent-of-empires/agent-of-empires (AoE v1.21.0)** | Rust (79%) \+ TypeScript, tmux session manager \+ Web UI | Claude Code, OpenCode, Pi.dev, Gemini CLI, Codex | Wraps each agent in a persistent tmux session \+ Git worktree. Great mobile web UI and status detection (working, waiting, idle). | **No Autonomous Loop:** AoE is a *human-driven multiplexer*. It does not decompose backlogs, route CI failures back to workers, or have agents drive other agents. |
| **2\. OrchestratorInc/agent-orchestrator** | Go daemon \+ Electron/React UI \+ Live Kanban | 30+ including **Claude Code, Pi, Muse, OpenCode 2** | Has a persistent "Project Orchestrator" agent that breaks plans into Kanban cards (Needs you, In review, Ready to merge), spawns workers in isolated Git worktrees, and routes PR/CI failures back to the worker. | **Desktop-First Coupling:** While a headless Docker workflow exists in CI, the primary supervisor UX is built around its local desktop app rather than a pure declarative server daemon. |
| **3\. sortie-ai/sortie (v1.30.0)** | Single Go binary (95% Go) \+ embedded SQLite (.sortie.db) \+ /ui/ Web Dashboard | Claude Code, OpenCode, Codex, Copilot, Kiro | Polls **GitHub Issues** (agent-ready label) via WORKFLOW.md (Go text/template). Clones worktree (after\_create), runs agent, opens PR (after\_run), swaps label to review. Has stall\_timeout\_ms (kills/requeues hung agents) and routes failing GitHub Actions CI checks back to the agent. | **Flat Ticket Runner:** Sortie is a relentless 1-ticket-to-1-PR state machine, but it has no concept of persistent multi-agent pods talking to each other (dev-cpp asking dev-csharp a question mid-task). |
| **4\. mvschwarz/openrig (v0.7.4)** | TypeScript/Hono daemon \+ SQLite \+ tmux \+ **Built-in MCP Server** | **Claude Code, Codex, Pi** | Declarative YAML RigSpec (pods, members, edges, CULTURE.md). Every agent gets a stable address (e.g., cpp-core@engine-rig). **Crucial Feature:** Exposes an MCP server (rig\_up, rig\_send, rig\_ps, rig\_snapshot\_create) *to the agents themselves*, so a Lead Agent can spawn, message, and inspect specialist agents over SQLite inbox/outbox queues. | **No Native GitHub Issue Poller / Business Kanban:** It excels at East-West inter-agent topology and tmux persistence, but relies on an operator or external trigger to feed high-level backlog items. |
| **5\. paperclipai/paperclip** | Node.js 24.11+ server \+ React UI \+ Postgres/SQLite | **Claude Code, Pi, Hermes, OpenCode, Codex**, HTTP/Process | **The "Virtual Company" Control Plane.** Organizes agents into an **Org Chart** (CEO \-\> Tech Lead \-\> Engineers) with **Atomic Task Checkout** (execution locks so two agents never claim the same ticket), **Heartbeat Loops** (HEARTBEAT.md wakes agents on a schedule to check their queue/subordinates), **Goal Ancestry**, and **Runtime Skill Injection**. | **High-Level Focus:** Manages the "Who, Why, and Task Lock" brilliantly, but doesn't manage low-level C++/C\# tmux sub-topologies as tightly as OpenRig. |

### **The Ultimate Unattended Architecture: The 3-Layer "Russian Doll" Stack**

Because **tokens are no issue** and you want this running inside an LXC container until the entire engine is built, **do not pick just one.** Combine the three that complement each other's exact mechanical boundaries:

┌─────────────────────────────────────────────────────────────────────────────┐  
│ LAYER 1: MACRO GOVERNANCE & KANBAN — Paperclip (paperclipai/paperclip)      │  
│ • Runs on Port 3000 (React Kanban \+ Org Chart \+ Heartbeat Scheduler)        │  
│ • Holds the 5 Milestones & 24 Engine Tasks with Goal Ancestry               │  
│ • Wakes the "Chief Architect" (Hermes Agent) every 10m via Heartbeat        │  
└──────────────────────────────────────┬──────────────────────────────────────┘  
                                       │  
         ┌─────────────────────────────┴─────────────────────────────┐  
         ▼ (Polls GitHub Issues / PRs)                               ▼ (Invokes \`rig send\` via MCP)  
┌─────────────────────────────────────────┐ ┌─────────────────────────────────┐  
│ LAYER 2A: CI/PR WATCHDOG — Sortie       │ │ LAYER 2B: SWARM TOPOLOGY —      │  
│ (sortie-ai/sortie single Go binary)     │ │ OpenRig (mvschwarz/openrig)     │  
│ • Polls GitHub Issues labeled           │ │ • Declarative YAML RigSpec      │  
│   \`agent-ready\`                         │ │ • Persistent tmux seats \+ SQLite│  
│ • Enforces \`stall\_timeout\_ms: 300000\`   │ │   Inbox/Outbox queues           │  
│ • Automatically routes failing CMake /  │ │ • Lead Agent uses OpenRig MCP   │  
│   dotnet test CI logs back to the       │ │   to drive 6 specialist Pi &    │  
│   exact branch worker until green       │ │   Hermes seats in parallel      │  
└─────────────────────────────────────────┘ └─────────────────────────────────┘

> 1. **Paperclip (paperclipai/paperclip)** is your **Top-Level Kanban & Heartbeat Engine**: It guarantees the system *never stops*. Even if every terminal session finishes a turn, Paperclip's heartbeat scheduler wakes up your Chief-Architect (running **Hermes**) and Milestone-Lead (running **Pi**) every 5–10 minutes with their HEARTBEAT.md instructions: *"Check open tasks, review blocked PRs, verify Milestone gates, and dispatch next sub-tasks."*  
> 2. **OpenRig (mvschwarz/openrig)** is your **Inter-Agent Mesh ("Agents Driving Agents")**: Your Milestone-Lead seat has OpenRig's MCP server (packages/cli/src/mcp-server.ts) attached. It uses rig send cpp-architect@engine-rig "..." and rig send roslyn-specialist@engine-rig "..." to coordinate cross-boundary work (like making sure engine\_abi.h in C++ matches NativeApi.g.cs in C\#).  
> 3. **Sortie (sortie-ai/sortie)** is your **Relentless Issue-to-PR Execution Loop**: Whenever the Architect marks a GitHub Issue as agent-ready, Sortie atomically checks it out into an isolated Git worktree, spawns the worker, monitors stall\_timeout\_ms (if an agent hangs on a CMake build or segfault, Sortie kills and retries with exponential backoff), opens the PR, and feeds any failing GitHub Action / test output right back into the agent until the PR is mergeable.

## **Part 3: Wiring the Memory Layer (unsloth/embeddinggemma-2-GGUF)**

Let's look at the actual specs of **unsloth/embeddinggemma-2-GGUF** (released Oct 2026):

* **Architecture:** 740M total parameters (or 270M text+code only sub-model), **8,192 token context window**, Matryoshka output dimensions (**768**, 512, 256, or 128).  
* **Critical Requirement (Do Not Miss This):** EmbeddingGemma 2 requires **L2 normalization** and **explicit task prefixes** on inputs (SearchQuery prefix for queries, Document prefix for indexed code/memories) to hit peak retrieval accuracy.

### **The 2-Tier Memory Architecture in Your LXC Container**

Assuming your embeddinggemma-2-GGUF is served via llama-server (or Lemonade/LiteLLM) with \--embeddings \--pooling mean exposing an OpenAI-compatible /v1/embeddings endpoint at http\://embedding-host:8080/v1:

> 1. **Semantic Codebase & Architecture Memory (Qdrant \+ Custom FastMCP Bridge):**  
   * Run a local **Qdrant** container/service (localhost:6333) inside the LXC.  
   * Why not standard mcp-server-qdrant? Because standard mcp-server-qdrant defaults to local FastEmbed CPU models (all-MiniLM-L6-v2) and doesn't inject EmbeddingGemma-2's mandatory query/document prefixes.  
   * Instead, run a 60-line Python FastMCP server (engine-memory-mcp) that hits your embeddinggemma-2-GGUF endpoint at **768 dimensions**, L2-normalizes the vector, and stores/queries two Qdrant collections:  
     * engine\_architecture\_memory: Architectural decisions, ABI contracts, Flecs/Jolt gotchas, and post-mortems of failed builds.  
     * engine\_code\_symbols: AST-chunked C++ headers and C\# interfaces indexed automatically by a Git post-commit hook.  
> 2. **Procedural & Episodic Memory (Hermes Native MEMORY.md \+ SQLite FTS5):**  
   * Hermes Agent natively maintains \~/.hermes/MEMORY.md (scanned for prompt injection before every write) and full-text FTS5 search over all historical agent transcripts (\~/.hermes/sessions.db).  
   * Symlink \~/.hermes/MEMORY.md into the shared workspace (/workspace/SHARED\_MEMORY.md) so every **Pi** worker seat in OpenRig reads the exact same curated institutional memory on startup.

## **Part 4: The Exact MCP Servers & Skills Stack**

To build a C++20 (CMake/Flecs/Jolt/Vulkan) \+ .NET 10 (Roslyn/ImGui) engine without agents hallucinating APIs or breaking the ABI, every agent seat needs a locked-in set of **LSPs**, **MCP servers**, and **Skills**.

### **1\. Language Servers (Wired into pi-lsp-extension & hermes-code-intel-plugin)**

Install these binaries directly in the LXC image so pi-lsp-extension auto-detects them and appends compiler errors after every file edit:

* **C++20 (clangd-19 or clangd-20):** Configured with \--background-index \--clang-tidy \--header-insertion=never \--compile-commands-dir=/workspace/build.  
* **C\# / .NET 10 (csharp-ls or Microsoft.CodeAnalysis.LanguageServer):** Installed via dotnet tool install \--global csharp-ls. Gives Pi instant diagnostics on unsafe blocks, Span\<T\>, and Roslyn IIncrementalGenerator code.  
* **Shaders (slangd / glsl\_analyzer):** Language server for .slang and .glsl compute/mesh shaders.

### **2\. The 6 Mandatory MCP Servers (.mcp.json)**

Attach these across your OpenRig / Paperclip seats:

> 1. **openrig-mcp (@openrig/cli built-in MCP):**  
   * Gives the Lead Agent rig\_up, rig\_down, rig\_ps, rig\_send, and rig\_snapshot\_create to command the worker seats.  
> 2. **clangd-mcp (uv tool install git+\[https\://github.com/schuay/clangd-mcp.git\](https\://github.com/schuay/clangd-mcp.git)):**  
   * Exposes C++ call hierarchies (get\_callers, get\_callees, find\_implementations, list\_file\_symbols) over compile\_commands.json for agents that need deep cross-TU C++ inspection beyond single-file diagnostics.  
> 3. **roslyn-mcp (JoshuaRamirez/RoslynMcpServer or sphcode/roslyn-mcp-server):**  
   * Exposes 60+ Roslyn semantic workspace tools (find all implementations of IComponent, inspect generated .g.cs syntax trees in memory, verify blittable struct layout).  
> 4. **ast-grep-mcp (\[github.com/ast-grep/ast-grep-mcp\](https\://github.com/ast-grep/ast-grep-mcp) or @notprolands/ast-grep-mcp):**  
   * Exposes dump\_syntax\_tree, find\_code, find\_code\_by\_rule, and test\_match\_code\_rule. Essential for bulk codemods across C++ and C\#.  
> 5. **engine-memory-mcp (Backed by unsloth/embeddinggemma-2-GGUF \+ Qdrant):**  
   * Exposes memory\_store\_decision, memory\_search\_architecture, and codebase\_semantic\_search.  
> 6. **renderdoc-vulkan-mcp (Headless GPU Verification Wrapper):**  
   * A lightweight local MCP wrapper around vulkaninfo, glslangValidator, spirv-Cross, and headless ctest runner (ctest \--output-on-failure) using **Lavapipe** (mesa-vulkan-drivers software rasterizer) or passthrough /dev/dri GPU so agents can run and screenshot Vulkan 1.3 tests inside the LXC container without a physical monitor.

### **3\. The Skills Library (\~/.agents/skills/ & \~/.claude/skills/)**

Install these skills into the shared skills directory so Pi, Hermes, and Claude Code load them automatically:

* **openrig/skills/rigs & refocusing**: Built-in OpenRig skills for inter-agent message passing (rig send), queue management, and context refocusing when a task drifts.  
* **paperclip-skill (paperclip/doc/SPEC.md)**: Allows agents to query goal ancestry, update ticket status, post build metrics, and spawn follow-up tasks in the Paperclip Kanban.  
* **hermes/ast-grep (nousresearch/hermes-agent/optional-skills/software-development/ast-grep/SKILL.md)**: Enforces the two-pass dry-run invariant (helper validate \-\> helper search \-\> helper replace \--apply).  
* **Custom Engine Domain Skills (We will seed these on boot):**  
  * zero-copy-abi-guard: Enforces that every change to native/bridge/engine\_abi.h is immediately followed by running python3 tools/abi\_codegen/generate.py and dotnet test on the layout assertions.  
  * flecs-jolt-patterns: Contains canonical C99/C++20 idioms for Flecs v4 ecs\_query\_t, alignas(64) component registration, and Jolt BodyInterface lock-free batching.  
  * roslyn-incremental-gen: Enforces strict IIncrementalGenerator value-equatable pipeline rules so generated code never breaks IDE responsiveness.

## **Part 5: How to Configure Muse Spark 1.3 Across All Harnesses**

Meta's Model API (\[https\://api.meta.ai/v1\](https\://api.meta.ai/v1)) is natively drop-in compatible with **both** the OpenAI API and the Anthropic API (muse-spark-1.3-contributor, 1M context window). Put these environment variables in /etc/environment inside the LXC container:

Bash  
\# Master Muse Spark 1.3 Contributor Credentials  
export MODEL\_API\_KEY="your\_meta\_model\_api\_key"

\# 1\. For OpenAI-compatible Harnesses (Pi, Hermes, OpenCode, Sortie)  
export OPENAI\_API\_BASE="https\://api.meta.ai/v1"  
export OPENAI\_BASE\_URL="https\://api.meta.ai/v1"  
export OPENAI\_API\_KEY="\${MODEL\_API\_KEY}"

\# 2\. For Anthropic-compatible Harnesses (Claude Code, Paperclip Default Adapter)  
export ANTHROPIC\_BASE\_URL="https\://api.meta.ai/v1"  
export ANTHROPIC\_AUTH\_TOKEN="\${MODEL\_API\_KEY}"  
export ANTHROPIC\_API\_KEY="\${MODEL\_API\_KEY}"  
export ANTHROPIC\_MODEL="muse-spark-1.3-contributor"  
export ANTHROPIC\_DEFAULT\_SONNET\_MODEL="muse-spark-1.3-contributor"  
export ANTHROPIC\_DEFAULT\_OPUS\_MODEL="muse-spark-1.3-contributor"  
export ANTHROPIC\_DEFAULT\_HAIKU\_MODEL="muse-spark-1.2"

\# 3\. For Unsloth EmbeddingGemma-2-GGUF Server  
export EMBEDDING\_BASE\_URL="http\://127.0.0.1:8081/v1" \# Or your Proxmox host IP  
export EMBEDDING\_MODEL="unsloth/embeddinggemma-2-GGUF"

## **Part 6: The Turnkey LXC Factory Blueprint (Generated Files)**

To make this immediately actionable, I am generating the 4 core configuration files you need to drop into your LXC container to start this factory and let it run until all 5 milestones are complete:

> 1. **lxc-factory/setup-lxc-factory.sh**: Complete provisioning script for an Ubuntu 24.04 Proxmox LXC container (installs C++20/Clang-19/CMake/Ninja/Vulkan-SDK/Lavapipe, .NET 10 SDK, Node 24, Go 1.23, Qdrant, pi-coding-agent \+ pi-lsp-extension, hermes-agent \+ hermes-code-intel-plugin, claude-code, openrig, sortie, and paperclip, plus systemd services so everything survives reboots).  
> 2. **lxc-factory/openrig-engine-team.yaml**: The declarative **OpenRig RigSpec** defining your 6 persistent agent seats (chief-architect, cpp-ecs-core, dotnet-roslyn-bridge, vulkan-renderer, editor-cli-ux, and adversarial-qa-gate), their harnesses (Pi \+ Hermes), and their inter-agent communication edges.  
> 3. **lxc-factory/WORKFLOW.md**: The **Sortie** autonomous issue-runner configuration that polls your GitHub repo for agent-ready issues, manages isolated Git worktrees, enforces a 5-minute stall watchdog, and loops failed CMake/.NET CI checks back to the worker until green.  
> 4. **lxc-factory/mcp-embeddinggemma-qdrant.py**: The custom FastMCP server that connects your agents' long-term memory directly to your **unsloth/embeddinggemma-2-GGUF** server with proper SearchQuery/Document prefixes and L2-normalized 768-dim vectors.

lxc-factory/setup-lxc-factory.sh \[generated with file\_gen tool\]  
lxc-factory/openrig-engine-team.yaml \[generated with file\_gen tool\]  
lxc-factory/WORKFLOW.md \[generated with file\_gen tool\]  
lxc-factory/mcp-embeddinggemma-qdrant.py \[generated with file\_gen tool\]

## **Part 7: How the Unattended Loop Actually Runs (From Boot to Completion)**

Once you run setup-lxc-factory.sh inside your Proxmox LXC container and run ./scripts/bootstrap-github-issues.sh (from our previous step) to populate the 24 GitHub issues, here is the exact mechanical loop that keeps running 24/7 until the engine is finished:

> 1. **The Heartbeat Trigger (Every 10 Minutes):**systemd runs engine-watchdog.timer (and Paperclip runs its internal heartbeat scheduler). If the openrig swarm is idle, it wakes chief-architect@engine-factory (running **Hermes Agent** on muse-spark-1.3-contributor).  
> 2. **Milestone Gating & Dispatch:**chief-architect queries engine-memory-mcp (backed by your embeddinggemma-2-GGUF server) to check what was completed in the last session, inspects the GitHub Issue board via gh issue list, and applies the agent-ready label to the next unblocked issues (e.g., M1-01 and M1-02).  
> 3. **Isolated Worktree Checkout & Execution:Sortie** (sortie-factory.service) detects the agent-ready label within 30 seconds, clones an isolated Git worktree, and launches **Pi** (pi-coding-agent) with WORKFLOW.md.  
> 4. **LSP-Guided Coding \+ Cross-Agent Consultation:**  
   * As the **Pi** worker writes C++20 or C\#13 code, pi-lsp-extension keeps clangd and csharp-ls hot in the background. Every file edit immediately returns compiler/type diagnostics in the same tool turn.  
   * If the C\# worker needs a change to the C99 ABI struct, it runs rig send cpp-ecs-core@engine-factory "Need EcsChunkView updated with entity\_ids pointer" over **OpenRig**.  
> 5. **Adversarial Verification & Self-Healing:** Before opening a PR, the worker must pass the local CMake \+ .NET 10 \+ Lavapipe headless Vulkan test suite. If a test fails or if an agent stalls for \> 5 minutes (stall\_timeout\_ms: 300000), Sortie kills the runaway process, feeds the failure log back into the retry queue, and loops until the PR is green.  
> 6. **Memory Consolidation & Skill Evolution:** Once adversarial-qa-gate@engine-factory approves and merges the PR, chief-architect stores the architectural summary into Qdrant via embeddinggemma-2-GGUF, runs hermes curator to refine its local SKILL.md files with any new Flecs/Jolt/Vulkan lessons learned, and unlocks the next issue on the Kanban board.

Would you like me to generate the **Paperclip Org Chart & Heartbeat seed script (seed-paperclip-org.js)** so Paperclip's UI on Port 	pre-loads these exact 6 agents and 5 milestones automatically on first boot, or should we generate the **Day-1 C++20/Flecs/.NET 10 seed repository files** so the agents have a clean initial commit to start from?