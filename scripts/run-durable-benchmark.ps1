param(
    [int]$TaskCount = 100,
    [int]$Trials = 5,
    [int]$TimeoutSeconds = 60,
    [string]$ApiUrl = "http://127.0.0.1:8000",
    [string]$DatabaseUrl = "postgresql://agentos:agentos@localhost:5432/agentos",
    [string]$PsqlPath = "C:\Program Files\PostgreSQL\17\bin\psql.exe",
    [string]$OutputPath = "benchmarks\results\durable_benchmark_latest.csv"
)

$ErrorActionPreference = "Stop"
if ($TaskCount -lt 1 -or $Trials -lt 1 -or $TimeoutSeconds -lt 1) {
    throw "TaskCount, Trials, and TimeoutSeconds must be positive"
}
if (-not (Test-Path -LiteralPath $PsqlPath)) {
    throw "psql was not found at $PsqlPath"
}

$databaseUri = [Uri]$DatabaseUrl
$credentials = $databaseUri.UserInfo.Split(':', 2)
$env:PGPASSWORD = $credentials[1]
$databaseUser = $credentials[0]
$databaseName = $databaseUri.AbsolutePath.TrimStart('/')
$rows = @()

1..$Trials | ForEach-Object {
    $trial = $_
    $prefix = "durable-bench-$(Get-Date -Format 'yyyyMMddHHmmssfff')-$trial"
    $tasks = 1..$TaskCount | ForEach-Object {
        @{
            id = "$prefix-$_"
            type = "sleep"
            payload = @{ seconds = 0 }
            dependencies = @()
        }
    }
    $body = @{ tasks = @($tasks) } | ConvertTo-Json -Depth 5 -Compress
    Invoke-RestMethod -Uri "$ApiUrl/tasks" -Method Post `
        -ContentType "application/json" -Body $body | Out-Null

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $states = & $PsqlPath -w -h $databaseUri.Host -p $databaseUri.Port `
            -U $databaseUser -d $databaseName -tA -c `
            "SELECT COUNT(*) FILTER (WHERE state = 'Completed'), COUNT(*) FILTER (WHERE state IN ('Failed', 'Blocked')) FROM tasks WHERE id LIKE '$prefix-%';"
        $stateCounts = $states.Split('|')
        if ([int]$stateCounts[1] -gt 0) {
            throw "Trial $trial has failed or blocked tasks"
        }
        if ((Get-Date) -gt $deadline) {
            throw "Trial $trial did not complete within $TimeoutSeconds seconds"
        }
        if ([int]$stateCounts[0] -lt $TaskCount) {
            Start-Sleep -Milliseconds 50
        }
    } while ([int]$stateCounts[0] -lt $TaskCount)

    $metrics = & $PsqlPath -w -h $databaseUri.Host -p $databaseUri.Port `
        -U $databaseUser -d $databaseName -tA -F ',' -c @"
WITH selected_tasks AS (
    SELECT * FROM tasks WHERE id LIKE '$prefix-%'
), starts AS (
    SELECT task_id, MIN(created_at) AS started_at
    FROM runtime_events
    WHERE task_id LIKE '$prefix-%' AND event_type = 'TaskStarted'
    GROUP BY task_id
), finishes AS (
    SELECT task_id, MAX(created_at) AS completed_at
    FROM runtime_events
    WHERE task_id LIKE '$prefix-%' AND event_type = 'TaskCompleted'
    GROUP BY task_id
)
SELECT COUNT(*),
       ROUND((COUNT(*) / EXTRACT(EPOCH FROM
           (MAX(f.completed_at) - MIN(t.created_at))))::numeric, 3),
       ROUND((percentile_cont(0.95) WITHIN GROUP (ORDER BY
           EXTRACT(EPOCH FROM (s.started_at - t.created_at)) * 1000))::numeric, 3),
       ROUND(EXTRACT(EPOCH FROM
           (MAX(f.completed_at) - MIN(t.created_at)))::numeric, 3)
FROM selected_tasks t
JOIN starts s ON s.task_id = t.id
JOIN finishes f ON f.task_id = t.id;
"@
    $values = $metrics.Split(',')
    $row = [pscustomobject]@{
        trial = $trial
        prefix = $prefix
        tasks = [int]$values[0]
        tasks_per_second = [decimal]$values[1]
        p95_ready_to_start_ms = [decimal]$values[2]
        elapsed_seconds = [decimal]$values[3]
    }
    $rows += $row
    $row | Format-Table -AutoSize
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force $outputDirectory | Out-Null
}
$rows | Export-Csv -NoTypeInformation -Path $OutputPath

$throughput = @($rows.tasks_per_second | Sort-Object)
$latency = @($rows.p95_ready_to_start_ms | Sort-Object)
$middle = [int][math]::Floor($Trials / 2)
if ($Trials % 2 -eq 0) {
    $medianThroughput = ($throughput[$middle - 1] + $throughput[$middle]) / 2
    $medianLatency = ($latency[$middle - 1] + $latency[$middle]) / 2
} else {
    $medianThroughput = $throughput[$middle]
    $medianLatency = $latency[$middle]
}

Write-Output "Median throughput: $medianThroughput tasks/sec"
Write-Output "Median trial p95 ready-to-start latency: $medianLatency ms"
Write-Output "Results written to $OutputPath"
