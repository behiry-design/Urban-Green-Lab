const fs = require("fs");
const {
  Document, Packer, Paragraph, TextRun, HeadingLevel, Table, TableRow, TableCell,
  WidthType, ShadingType, AlignmentType, BorderStyle, PageBreak,
} = require("docx");

// ---- Palette: matches the rest of the Smart Greenhouse project's documents ----
const GREEN = "1B4332";
const GREEN_MID = "52796F";
const AMBER = "B8791A";   // darkened from the deck's E0A458 for readable body-text contrast
const RED = "9E2A2B";
const CREAM = "F4F1EA";
const GRAY = "666666";
const CODE_BG = "F2F1EC";

function h1(text) {
  return new Paragraph({ text, heading: HeadingLevel.HEADING_1, spacing: { before: 320, after: 140 } });
}
function h2(text) {
  return new Paragraph({ text, heading: HeadingLevel.HEADING_2, spacing: { before: 220, after: 100 } });
}
function p(text, opts = {}) {
  return new Paragraph({ spacing: { after: 140, line: 268 }, children: [new TextRun({ text, ...opts })] });
}
function pRuns(runs, opts = {}) {
  return new Paragraph({ spacing: { after: 140, line: 268 }, ...opts, children: runs });
}
function bullet(text, opts = {}) {
  return new Paragraph({ text, bullet: { level: 0 }, spacing: { after: 90, line: 260 }, ...opts });
}
function numberedStep(n, title, body) {
  return [
    new Paragraph({
      spacing: { before: 160, after: 40 },
      children: [
        new TextRun({ text: `Step ${n}. `, bold: true, color: GREEN }),
        new TextRun({ text: title, bold: true }),
      ],
    }),
    p(body),
  ];
}
function codeBlock(lines) {
  return new Table({
    width: { size: 9000, type: WidthType.DXA },
    columnWidths: [9000],
    rows: [new TableRow({ children: [new TableCell({
      shading: { type: ShadingType.CLEAR, fill: CODE_BG },
      margins: { top: 140, bottom: 140, left: 200, right: 200 },
      children: lines.map((line, i) => new Paragraph({
        spacing: { after: i === lines.length - 1 ? 0 : 40 },
        children: [new TextRun({ text: line, font: "Courier New", size: 19 })],
      })),
    })] })],
  });
}
function noteBox(label, text, color) {
  return new Table({
    width: { size: 9000, type: WidthType.DXA },
    columnWidths: [9000],
    rows: [new TableRow({ children: [new TableCell({
      shading: { type: ShadingType.CLEAR, fill: CREAM },
      margins: { top: 140, bottom: 140, left: 200, right: 200 },
      children: [
        new Paragraph({ spacing: { after: 60 }, children: [new TextRun({ text: label, bold: true, color })] }),
        new Paragraph({ children: [new TextRun({ text })] }),
      ],
    })] })],
  });
}
function pageBreak() { return new Paragraph({ children: [new PageBreak()] }); }

const headerRow = (cells, widths) => new TableRow({
  children: cells.map((c, i) => new TableCell({
    shading: { type: ShadingType.CLEAR, fill: GREEN },
    margins: { top: 90, bottom: 90, left: 100, right: 100 },
    children: [new Paragraph({ children: [new TextRun({ text: c, bold: true, color: "FFFFFF", size: 18 })] })],
  })),
});
function dataRow(cells) {
  return new TableRow({ children: cells.map(c => new TableCell({
    margins: { top: 90, bottom: 90, left: 100, right: 100 },
    children: [new Paragraph({ children: [new TextRun({ text: c, size: 19 })] })],
  })) });
}

const doc = new Document({
  styles: { default: { document: { run: { font: "Calibri", size: 22 } } } },
  sections: [{
    properties: { page: { size: { width: 12240, height: 15840 } } }, // US Letter
    children: [
      // ---------------------------------------------------------------- Cover
      new Paragraph({ spacing: { after: 40 }, children: [new TextRun({ text: "SMART GREENHOUSE TRAINING PROGRAM", bold: true, size: 20, color: AMBER, characterSpacing: 20 })] }),
      new Paragraph({ spacing: { after: 200 }, children: [new TextRun({ text: "Platform Setup Guide", bold: true, size: 44, color: GREEN })] }),
      new Paragraph({ spacing: { after: 400 }, children: [new TextRun({ text: "Shared dashboard, sensor ingestion, and the pooled \"swarm\" health model — everything you set up once, before Day 1 of training.", italics: true, size: 22, color: GRAY })] }),

      p("This guide walks through standing up the platform that every training kit will report to: a free Supabase backend, a live web dashboard for you and the teams, and a pooled health model that gets smarter as more kits come online. Budget about 30-45 minutes end to end, done once before training starts."),

      h1("1. What you're setting up"),
      p("Four pieces work together. You'll set up the first three before training; teams interact with the dashboard during and after."),
      bullet("Backend (Supabase, free tier) — a database that holds every kit's readings and photos, plus the security rules that keep one team's writes from touching another's."),
      bullet("Kit firmware (ESP32, Arduino) — the template each team customizes with their WiFi and kit credentials, then flashes onto their own hardware after training."),
      bullet("Pooled health model (Python script) — pools every reporting kit's sensor data together and flags which kits look healthy, worth watching, or stressed. Run it by hand or on a schedule."),
      bullet("Dashboard (one static HTML file) — an instructor view of every kit at a glance, and a per-team view with live charts and a photo-upload form for documenting growth."),

      noteBox(
        "Scope, honestly: ",
        "there's no per-student login system here — each team gets a short Kit ID and a device token (like a password) printed on their kit card, and that's what tells kits apart. That's a deliberate trade for a 3-day training: it keeps setup and onboarding fast, at the cost of not being hardened against a determined bad actor. It's the right trade for a trusted classroom setting; see Section 11 before reusing this for anything higher-stakes.",
        GREEN,
      ),

      h1("2. Before you start"),
      bullet("A free Supabase account (supabase.com) — no credit card required at this scale."),
      bullet("The four project files: sql/setup.sql, firmware/kit_firmware.ino, swarm_model/train_pooled_model.py, dashboard/index.html."),
      bullet("Python 3 with pip, for running the health-model script (on your laptop, or any machine that can reach the internet during training)."),
      bullet("A rough headcount of teams (15-30) so you know how many Kit IDs to generate."),

      h1("3. Create the Supabase project"),
      ...numberedStep(1, "Sign up / log in", "Go to supabase.com and create a free account (GitHub login is fastest)."),
      ...numberedStep(2, "New project", "Click \"New project\", give it a name (e.g. \"greenhouse-training\"), set a database password (save it somewhere — you'll need it again in Section 9), and pick the region closest to campus. Click \"Create new project\" and wait 1-2 minutes for it to provision."),

      h1("4. Run the setup script"),
      ...numberedStep(1, "Open the SQL editor", "In your new project, go to the SQL Editor in the left sidebar and click \"New query\"."),
      ...numberedStep(2, "Paste and run", "Open sql/setup.sql, copy the whole file, paste it into the query editor, and click \"Run\". You should see a series of CREATE TABLE / CREATE POLICY / CREATE FUNCTION confirmations and no errors."),
      p("This one script creates every table, security rule, and helper function the platform needs — you only run it once. It's been tested end-to-end against a real Postgres database (including the security checks) before being handed to you."),
      noteBox("If something errors: ", "re-running the whole script is safe — every statement uses \"if not exists\" / \"or replace\" where it matters, so re-running it after fixing a typo won't duplicate anything.", GREEN_MID),

      h1("5. Register your teams' kits"),
      p("Each team needs a short Kit ID (e.g. K01, K02, ...) and a random device token that only that team knows. Generate one token per team:"),
      codeBlock(["python3 -c \"import secrets; print(secrets.token_hex(8))\""]),
      p("Run that once per team and keep the results in a private spreadsheet (Kit ID, team name, fruit type, token) — you'll print one row per team onto their kit card in Section 8. Then, back in the Supabase SQL editor, register all the teams in one query:"),
      codeBlock([
        "insert into kits (id, team_name, fruit_type, device_token) values",
        "  ('K01', 'Team Falcon', 'strawberry', 'paste-team-1-token-here'),",
        "  ('K02', 'Team Basil',  'tomato',     'paste-team-2-token-here'),",
        "  ('K03', 'Team Cedar',  'cucumber',   'paste-team-3-token-here');",
        "  -- one row per team, up to K30",
      ]),
      noteBox("Keep the token list private: ", "a device token is effectively a team's password for writing data. Hand each team ONLY their own row, printed on their kit card — not the full spreadsheet.", RED),

      h1("6. Get your API keys"),
      p("In your Supabase project, go to Project Settings > API. You need two values from that page:"),
      bullet("Project URL — looks like https://abcdefgh.supabase.co"),
      bullet("anon / public key — a long string starting with \"eyJ...\". This is safe to put in a public file; it's not a secret (that's what device tokens are for)."),
      p("Open dashboard/index.html in a text editor and find the CONFIG block near the top of the <script> section. Paste your Project URL and anon key in:"),
      codeBlock([
        "const CONFIG = {",
        "  SUPABASE_URL:      \"https://abcdefgh.supabase.co\",",
        "  SUPABASE_ANON_KEY: \"eyJ...your-anon-key...\",",
        "  ...",
        "};",
      ]),
      p("These same two values also go into every team's firmware config (Section 8) — they're the same for every kit, unlike the Kit ID/token pair."),

      h1("7. Deploy the dashboard"),
      p("The dashboard is a single HTML file with no build step — the fastest way to get it a public link is Netlify's drag-and-drop deploy:"),
      ...numberedStep(1, "Go to app.netlify.com/drop", "No account needed for a quick drop deploy (creating a free account lets you update it later instead of getting a new link each time)."),
      ...numberedStep(2, "Drag the dashboard folder in", "Drag the whole dashboard/ folder (containing index.html) onto the page. Netlify gives you a live URL in seconds — that's the link you share with every team."),
      noteBox(
        "Alternative for same-room use only: ",
        "if the training happens in one room and a public link isn't needed, you can skip deployment entirely and just open dashboard/index.html directly in a browser on a laptop connected to a projector — it works the same way, it's just not reachable from students' own devices.",
        GREEN_MID,
      ),

      h1("8. Prepare each team's kit card"),
      p("Print or hand-write one card per team with everything they need:"),
      new Table({
        width: { size: 9000, type: WidthType.DXA },
        columnWidths: [2200, 2200, 2300, 2300],
        rows: [
          headerRow(["Kit ID", "Team / fruit", "Device token", "WiFi + dashboard link"]),
          dataRow(["K01", "Team Falcon\n(strawberry)", "(their token)", "(shared WiFi + dashboard URL)"]),
        ],
      }),
      p("During training, each team edits the CONFIG block at the top of firmware/kit_firmware.ino with their WiFi credentials, their Kit ID and token, and the same SUPABASE_URL / SUPABASE_ANON_KEY from Section 6 — that part of the firmware config is identical across every kit. The firmware file itself is already written and commented for a DHT22 + capacitive soil probe; the calibration section of your training material covers measuring SOIL_DRY_RAW / SOIL_WET_RAW for their specific probe."),

      pageBreak(),

      h1("9. Running the pooled \"swarm\" health model"),
      p("This is the piece that makes the platform more than a bunch of separate dashboards: it pools every reporting kit's sensor data together, so a kit's readings get judged against the whole class rather than a fixed threshold nobody chose. One team's kit alone can't tell you what \"normal\" looks like — fifteen to thirty kits together can."),
      ...numberedStep(1, "Get your database connection string", "In Supabase: Project Settings > Database > Connection string (URI). Copy it — it includes the database password you set in Section 3."),
      ...numberedStep(2, "Install dependencies (once)", "On the machine that will run the script:"),
      codeBlock(["pip install pandas numpy scikit-learn psycopg2-binary"]),
      ...numberedStep(3, "Run it", ""),
      codeBlock([
        "export SUPABASE_DB_URL=\"postgresql://postgres:[password]@[host]:5432/postgres\"",
        "python3 train_pooled_model.py",
      ]),
      p("It prints a score/status line per kit and writes the same results to the health_scores table, which the dashboard reads immediately. Re-run it any time — every run recomputes from scratch, so it's always safe to re-run. During training, running it every 15-30 minutes (by hand, or as a cron job / scheduled task) keeps everyone's status current."),
      noteBox(
        "Why a kit sometimes shows \"no data yet\": ",
        "the model needs at least 3 readings from a kit in the last 24 hours before it will score it at all — a single reading isn't enough to judge, and this avoids one noisy sample looking like a crisis.",
        GREEN_MID,
      ),

      h1("10. Day-of checklist"),
      bullet("Dashboard link works and shows the instructor view with all registered kits (even if most say \"no data yet\")."),
      bullet("At least one test kit has sent a reading successfully — flash the firmware onto one kit ahead of time and confirm it reaches the dashboard before relying on it live in front of everyone."),
      bullet("Kit cards printed and ready to hand out, one per team, with nothing but that team's own Kit ID/token."),
      bullet("The health-model script has been run at least once (it's fine if every kit still shows \"no data yet\" before kits are built — that's expected on Day 1)."),
      bullet("You know how to re-run the health model quickly (Section 9) — worth demonstrating live once a few kits are reporting, so students see the group's data improving everyone's status."),

      h1("11. Scope & limitations — read before reusing this beyond training"),
      p("A few deliberate simplifications kept this buildable in the time available. They're fine for a short, trusted, in-person training; they're worth revisiting before reusing this setup for anything longer-running or higher-stakes:"),
      bullet("No per-student accounts — teams are told apart by a shared Kit ID + device token, not individual logins. Anyone with a team's pair can act as that team."),
      bullet("The photo storage bucket accepts uploads from anyone with the dashboard's (public, non-secret) anon key — the token check happens when the photo is registered in the gallery, not on the raw file upload itself, so a stray unregistered file could in principle land in storage without showing up anywhere. Low-consequence, but worth knowing."),
      bullet("The ESP32 firmware skips TLS certificate verification (client.setInsecure()) for simplicity. Fine for a short training deployment; a hardened long-term deployment should pin Supabase's certificate instead."),
      bullet("The pooled health model is centralized — it pools everyone's raw data on one machine and retrains from scratch each run. That's what \"swarm\" means here: shared intelligence from pooled data, not literal decentralized/on-device training."),

      h1("12. Upgrade path: true on-device federated learning"),
      p("The pooled model above is the right starting point — it delivers real value from day one with no hardware beyond an ESP32. If the program continues past this training and teams' kits grow Raspberry Pis with real compute, a natural next step is genuine federated learning: each kit trains a small local model on its own data and shares only model updates (never raw data) with a coordinator that averages them — frameworks like Flower (flwr) are built for exactly this on Raspberry Pi-class hardware. That's a meaningfully bigger engineering lift than the pooled model here (a coordinator service, client code on every Pi, a network topology that tolerates kits going offline), so it's listed as a deliberate stretch goal rather than something to attempt before Day 1."),

      h1("13. What's in the project files"),
      new Table({
        width: { size: 9000, type: WidthType.DXA },
        columnWidths: [2600, 6400],
        rows: [
          headerRow(["File", "Purpose"]),
          dataRow(["sql/setup.sql", "Run once in Supabase's SQL editor. Creates all tables, security rules, and the token-checked write functions."]),
          dataRow(["firmware/kit_firmware.ino", "Arduino sketch each team customizes and flashes onto their ESP32."]),
          dataRow(["swarm_model/train_pooled_model.py", "Pools every kit's readings, scores each kit's health, writes results back."]),
          dataRow(["dashboard/index.html", "The instructor + per-team web dashboard. Deploy as-is (Section 7)."]),
        ],
      }),
    ],
  }],
});

Packer.toBuffer(doc).then((buf) => {
  fs.writeFileSync("Training_Platform_Setup_Guide.docx", buf);
  console.log("Guide written.");
});
