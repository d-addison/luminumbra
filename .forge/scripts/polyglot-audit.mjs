import fs from "fs";
import path from "path";

const root = process.cwd();
const manifestPath = path.join(root, ".forge", "polyglot.json");
const reportDir = path.join(root, ".forge", "reports");
const jsonReportPath = path.join(reportDir, "polyglot-audit.json");
const markdownReportPath = path.join(reportDir, "polyglot-audit.md");

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

function toPosix(value) {
  return value.split(path.sep).join("/");
}

function rel(filePath) {
  return toPosix(path.relative(root, filePath));
}

function isExcluded(relativePath, excludes) {
  const normalized = toPosix(relativePath);
  return excludes.some((entry) => {
    const exclude = toPosix(entry).replace(/\/+$/, "");
    return normalized === exclude || normalized.startsWith(`${exclude}/`);
  });
}

function walk(startPath, excludes, out) {
  const absolute = path.join(root, startPath);
  if (!fs.existsSync(absolute)) {
    return;
  }

  const stat = fs.statSync(absolute);
  const relative = rel(absolute);
  if (relative && isExcluded(relative, excludes)) {
    return;
  }

  if (stat.isFile()) {
    out.push(absolute);
    return;
  }

  if (!stat.isDirectory()) {
    return;
  }

  for (const entry of fs.readdirSync(absolute)) {
    walk(path.join(startPath, entry), excludes, out);
  }
}

function languageFor(filePath, manifest) {
  const base = path.basename(filePath);
  for (const [language, names] of Object.entries(manifest.special_files || {})) {
    if (names.includes(base)) {
      return language;
    }
  }

  const ext = path.extname(filePath);
  for (const [language, extensions] of Object.entries(manifest.languages)) {
    if (extensions.includes(ext)) {
      return language;
    }
  }

  return "other";
}

function countCodeLines(content) {
  return content
    .split(/\r?\n/)
    .filter((line) => {
      const trimmed = line.trim();
      return trimmed.length > 0 && !trimmed.startsWith("//");
    }).length;
}

function moduleFor(relativePath, modules) {
  for (const module of modules) {
    if (module.paths.some((prefix) => relativePath === prefix || relativePath.startsWith(`${prefix}/`))) {
      return module.name;
    }
  }
  return "unmapped";
}

function collectFiles(manifest) {
  const files = [];
  for (const rootPath of manifest.roots) {
    walk(rootPath, manifest.exclude, files);
  }
  return [...new Set(files)].sort();
}

function scanRisks(relativePath, language, content, manifest) {
  const findings = [];
  const lines = content.split(/\r?\n/);

  for (const pattern of manifest.risk_patterns) {
    if (pattern.languages && !pattern.languages.includes(language)) {
      continue;
    }

    const regex = new RegExp(pattern.regex);
    lines.forEach((line, index) => {
      const trimmed = line.trim();
      if (pattern.id !== "todo" && (trimmed.startsWith("//") || trimmed.startsWith("*"))) {
        return;
      }

      if (regex.test(line)) {
        findings.push({
          id: pattern.id,
          severity: pattern.severity,
          file: relativePath,
          line: index + 1,
          text: trimmed.slice(0, 220),
          description: pattern.description
        });
      }
    });
  }

  return findings;
}

function detectTests(files) {
  const testFiles = [];
  let gtestCases = 0;
  let gtestDiscover = false;

  for (const file of files) {
    const relativePath = rel(file);
    const content = fs.readFileSync(file, "utf8");

    if (relativePath.startsWith("test/") && /\.(c|cc|cpp|cxx|h|hpp)$/.test(relativePath)) {
      testFiles.push(relativePath);
    }

    const matches = content.match(/\bTEST(_F|_P)?\s*\(/g);
    if (matches) {
      gtestCases += matches.length;
    }

    if (content.includes("gtest_discover_tests")) {
      gtestDiscover = true;
    }
  }

  return {
    files: testFiles.sort(),
    gtest_cases: gtestCases,
    gtest_discover_tests: gtestDiscover
  };
}

function summarizeBy(items, key) {
  const result = {};
  for (const item of items) {
    const value = item[key] || "unknown";
    result[value] = (result[value] || 0) + 1;
  }
  return result;
}

function topFiles(files, field, limit) {
  return files
    .filter((file) => file[field] > 0)
    .sort((a, b) => b[field] - a[field])
    .slice(0, limit)
    .map((file) => ({ file: file.path, [field]: file[field] }));
}

function renderMarkdown(report) {
  const languageRows = Object.entries(report.summary.by_language)
    .sort((a, b) => b[1].loc - a[1].loc)
    .map(([language, stats]) => `| ${language} | ${stats.files} | ${stats.loc} |`)
    .join("\n");

  const moduleRows = Object.entries(report.summary.by_module)
    .sort((a, b) => b[1].loc - a[1].loc)
    .map(([module, stats]) => `| ${module} | ${stats.files} | ${stats.loc} |`)
    .join("\n");

  const riskRows = Object.entries(report.summary.risks_by_id)
    .sort((a, b) => b[1] - a[1])
    .map(([id, count]) => `| ${id} | ${count} |`)
    .join("\n");

  const testRows = report.tests.files.map((file) => `- ${file}`).join("\n") || "- none detected";

  const topRows = report.summary.top_files_by_loc
    .map((entry) => `| ${entry.file} | ${entry.loc} |`)
    .join("\n");

  const recommendations = report.recommendations.map((item) => `- ${item}`).join("\n");

  return `# Polyglot Audit Report

Generated: ${report.generated_at}

## Summary

- Files scanned: ${report.summary.files}
- Source lines scanned: ${report.summary.loc}
- Languages detected: ${Object.keys(report.summary.by_language).length}
- Risk findings: ${report.risks.length}
- GoogleTest cases detected: ${report.tests.gtest_cases}
- CTest discovery wired: ${report.tests.gtest_discover_tests ? "yes" : "no"}

## Languages

| Language | Files | LOC |
|----------|-------|-----|
${languageRows}

## Modules

| Module | Files | LOC |
|--------|-------|-----|
${moduleRows}

## Risk Markers

| Pattern | Findings |
|---------|----------|
${riskRows || "| none | 0 |"}

## Tests

${testRows}

## Largest Files

| File | LOC |
|------|-----|
${topRows}

## Recommendations

${recommendations}
`;
}

function main() {
  const manifest = readJson(manifestPath);
  fs.mkdirSync(reportDir, { recursive: true });

  const discovered = collectFiles(manifest);
  const files = [];
  const risks = [];

  for (const file of discovered) {
    const relativePath = rel(file);
    const language = languageFor(file, manifest);
    if (language === "other") {
      continue;
    }

    let content = "";
    try {
      content = fs.readFileSync(file, "utf8");
    } catch {
      continue;
    }

    const loc = countCodeLines(content);
    const module = moduleFor(relativePath, manifest.modules);
    files.push({ path: relativePath, language, module, loc });
    risks.push(...scanRisks(relativePath, language, content, manifest));
  }

  const byLanguage = {};
  const byModule = {};
  for (const file of files) {
    byLanguage[file.language] ||= { files: 0, loc: 0 };
    byLanguage[file.language].files += 1;
    byLanguage[file.language].loc += file.loc;

    byModule[file.module] ||= { files: 0, loc: 0 };
    byModule[file.module].files += 1;
    byModule[file.module].loc += file.loc;
  }

  const tests = detectTests(files.map((file) => path.join(root, file.path)));
  const report = {
    schema_version: 1,
    generated_at: new Date().toISOString(),
    manifest: rel(manifestPath),
    test_commands: manifest.test_commands,
    summary: {
      files: files.length,
      loc: files.reduce((sum, file) => sum + file.loc, 0),
      by_language: byLanguage,
      by_module: byModule,
      risks_by_id: summarizeBy(risks, "id"),
      risks_by_severity: summarizeBy(risks, "severity"),
      top_files_by_loc: topFiles(files, "loc", 12)
    },
    tests,
    risks,
    modules: manifest.modules,
    recommendations: [
      "Treat CMake and CTest as first-class verification inputs for this repo.",
      "Keep vendor, external, and build outputs excluded from Forge pre-review scans.",
      "Promote the SHIELD world-generation test failures to the first remediation spec.",
      "Move Instinct Engine and Aetheric Field from README pillars into explicit C++ module specs before expanding implementation.",
      "Use this report as the project-local baseline until Forge core gets native C/C++ audit support."
    ]
  };

  fs.writeFileSync(jsonReportPath, `${JSON.stringify(report, null, 2)}\n`);
  fs.writeFileSync(markdownReportPath, renderMarkdown(report));

  console.log(`polyglot audit complete: ${report.summary.files} files, ${report.summary.loc} LOC, ${risks.length} risk findings`);
  console.log(`wrote ${rel(jsonReportPath)}`);
  console.log(`wrote ${rel(markdownReportPath)}`);
}

main();
