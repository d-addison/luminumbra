# Rewrites the retired gpt-5.3-codex model id (hardcoded by the dispatch
# spawner) to gpt-5.5, which ChatGPT-account Codex CLI supports.
$rewritten = $args | ForEach-Object { "$_" -replace 'gpt-5\.3-codex', 'gpt-5.5' }
& "$env:APPDATA\npm\codex.ps1" @rewritten
exit $LASTEXITCODE
