<#
.SYNOPSIS
Starts a reverse SSH tunnel from a Windows lab PC to the owner's SSH server.

.DESCRIPTION
Provision OpenSSH Server on the lab PC first. Enroll this PC's tunnel public
key on the destination SSH server and provision its verified host key in the
specified known_hosts file. Keep the destination's GatewayPorts set to no
so the forwarded port is only reachable from the destination itself.

Use a different RelayPort for each lab PC. On the destination, connect to
127.0.0.1:RelayPort using the lab PC's SSH account and verify its host key.
The tunnel runs in the foreground and closes when the SSH client exits.
Use -ValidateOnly to print effective client settings without connecting.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9.-]*\z')]
    [string] $RelayHost,

    [Parameter(Mandatory)]
    [ValidatePattern('^[a-zA-Z0-9_][a-zA-Z0-9_.-]*\z')]
    [string] $RelayUser,

    [Parameter(Mandatory)]
    [ValidateRange(1024, 65535)]
    [int] $RelayPort,

    [Parameter(Mandatory)]
    [string] $IdentityFile,

    [Parameter(Mandatory)]
    [string] $KnownHostsFile,

    [ValidateRange(1, 65535)]
    [int] $RelaySshPort = 22,

    [ValidateRange(1, 65535)]
    [int] $LocalSshPort = 22,

    [switch] $ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ssh = Get-Command ssh.exe -CommandType Application

function Get-SshConfigPath([string] $Path) {
    $item = Get-Item -LiteralPath $Path
    if ($item -isnot [System.IO.FileInfo]) {
        throw "Expected a file at '$Path'."
    }
    $configPath = $item.FullName.Replace('\', '/')
    if ($configPath -match '["%$\r\n~]') {
        throw "This path contains a character reserved by the SSH config: $Path"
    }
    return $configPath
}

$identityPath = Get-SshConfigPath $IdentityFile
$knownHostsPath = Get-SshConfigPath $KnownHostsFile

if (-not $ValidateOnly) {
    $localSocket = [System.Net.Sockets.TcpClient]::new()
    try {
        if (-not $localSocket.ConnectAsync('127.0.0.1', $LocalSshPort).Wait(3000)) {
            throw "Timed out connecting to 127.0.0.1:$LocalSshPort."
        }
    }
    catch {
        throw "Start the lab PC's SSH server on 127.0.0.1:$LocalSshPort first."
    }
    finally {
        $localSocket.Dispose()
    }
}

$config = New-TemporaryFile
try {
    $configText = @"
Host uno-lab-relay
    HostName $RelayHost
    Port $RelaySshPort
    User $RelayUser
    IdentityFile "$identityPath"
    UserKnownHostsFile "$knownHostsPath"
    GlobalKnownHostsFile none
    IdentitiesOnly yes
    BatchMode yes
    PreferredAuthentications publickey
    StrictHostKeyChecking yes
    ExitOnForwardFailure yes
    ConnectTimeout 10
    ServerAliveInterval 20
    ServerAliveCountMax 3
    RemoteForward 127.0.0.1:${RelayPort} 127.0.0.1:${LocalSshPort}
"@
    [System.IO.File]::WriteAllText($config.FullName, $configText, [System.Text.UTF8Encoding]::new($false))

    if ($ValidateOnly) {
        & $ssh.Source -F $config.FullName -G -N -T uno-lab-relay
    }
    else {
        Write-Host "Opening lab tunnel to ${RelayHost}:${RelaySshPort} (forwarded port $RelayPort). Ctrl+C closes it."
        & $ssh.Source -F $config.FullName -N -T uno-lab-relay
    }
    if ($LASTEXITCODE -ne 0) {
        throw "OpenSSH exited with code $LASTEXITCODE."
    }
}
finally {
    Remove-Item -LiteralPath $config.FullName -Force
}
