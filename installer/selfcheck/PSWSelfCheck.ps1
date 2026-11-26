<#
PSWSelfCheck.ps1 v1.7 (v1.7: -ChecksOnly [-ResultFile] for the PSWloader Dashboard; v1.6: console identity, ps5pwr live log, driver parameters, start history, summary.json, anonymized zip; v1.5: -Gui - window in the PSWloader look with progress, then Open folder / Open an issue on GitHub; Start menu "PSWloader Support"; v1.4: NVMe of any vendor; v1.3: SetupComplete and specialize logs; v1.2: memory integrity / VBS state in the summary) - collects everything support needs into one zip.
Writes C:\PSWloader\selfcheck\results\PSW-selfcheck-<yyyy-MM-dd_HH-mm-ss>.zip and opens the folder.
Read-only on the system; the only change is a temporary drive letter for the EFI system partition (removed again).
Run as administrator (PSWSelfCheck.cmd does that).
#>
param([switch]$NoExplorer, [switch]$Gui, [string]$ProgressFile = '', [string]$Preview = '', [switch]$ChecksOnly, [string]$ResultFile = '')
# v1.5 (user 26.11.2026): progress for the window - "<percent><TAB><text>" per line, "ZIP<TAB><path>" at the end
function Prog([int]$Pct, [string]$Text) { if ($ProgressFile) { try { Add-Content -LiteralPath $ProgressFile -Value "$Pct`t$Text" -Encoding UTF8 } catch { } } }
if ($ProgressFile -and -not $Gui) { try { [IO.File]::WriteAllText($ProgressFile, '') } catch { } }   # v1.8: every run starts its progress file empty (the Dashboard reads it)

if ($Gui -or $Preview) {
  # the window (same look as the PSWloader windows); the collection runs as a hidden child process of this script
  if (-not $Preview -and -not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    try { Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -Gui" } catch { }
    exit 0
  }
  Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
  try { [System.Windows.Media.RenderOptions]::ProcessRenderMode = [System.Windows.Interop.RenderMode]::SoftwareOnly } catch { }   # v1.2 (HW 26.11.): software rendering, independent of the GPU driver
  function Set-PswDarkTitle($Window) {   # v1.2 (user 26.11.2026): PSWloader windows have no Windows title bar (PowerShell icon,
    # title text) - a frameless window with its own minimize/close buttons at the top right, moved by dragging any free area;
    # the PSWloader logo with an own identity in the taskbar; in front when it appears (Windows' foreground lock kept windows
    # of background tasks behind others - a short Alt key press lets SetForegroundWindow through)
    try { $pre = "$env:ProgramFiles\PSWloader\PswDwm.dll"   # compiled once by PSWSetup (component Dashboard) - saves about half a second of compiling at every start
          if (-not ('PswDwm.Native' -as [type]) -and (Test-Path -LiteralPath $pre)) { try { Add-Type -Path $pre } catch { } }
          if (-not ('PswDwm.Native' -as [type])) { Add-Type -Namespace PswDwm -Name Native -MemberDefinition @'
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(System.IntPtr h, int a, ref int v, int s);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);
[DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, System.UIntPtr e);
[DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern int SetCurrentProcessExplicitAppUserModelID(string id);
'@ } } catch { }
    try { [void][PswDwm.Native]::SetCurrentProcessExplicitAppUserModelID('PSWloader.Windows')   # own taskbar entry, not PowerShell's
          if ($null -eq $Window.Icon) {
            $png = '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'
            $Window.Icon = [System.Windows.Media.Imaging.BitmapFrame]::Create((New-Object IO.MemoryStream(, [Convert]::FromBase64String($png))), [System.Windows.Media.Imaging.BitmapCreateOptions]::None, [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad) } } catch { }
    try {
      $min = ($Window.ResizeMode -ne 'NoResize')
      $Window.WindowStyle = 'None'; $Window.ResizeMode = $(if ($min) { 'CanMinimize' } else { 'NoResize' })
      $inner = $Window.Content; $Window.Content = $null
      $g = New-Object System.Windows.Controls.Grid; [void]$g.Children.Add($inner)
      $bar = New-Object System.Windows.Controls.StackPanel; $bar.Orientation = 'Horizontal'; $bar.HorizontalAlignment = 'Right'; $bar.VerticalAlignment = 'Top'
      foreach ($d in @(@('min', 0xE921), @('close', 0xE8BB))) {
        if ($d[0] -eq 'min' -and -not $min) { continue }
        $b = New-Object System.Windows.Controls.Border; $b.Width = 46; $b.Height = 32; $b.Background = [System.Windows.Media.Brushes]::Transparent; $b.Tag = $d[0]
        $t = New-Object System.Windows.Controls.TextBlock; $t.Text = [string][char]$d[1]; $t.FontFamily = 'Segoe MDL2 Assets'; $t.FontSize = 10
        $t.Foreground = [System.Windows.Media.Brushes]::Gainsboro; $t.HorizontalAlignment = 'Center'; $t.VerticalAlignment = 'Center'; $b.Child = $t
        $b.Add_MouseEnter({ $this.Background = (New-Object System.Windows.Media.BrushConverter).ConvertFromString($(if ($this.Tag -eq 'close') { '#C42B1C' } else { '#353535' })) })
        $b.Add_MouseLeave({ $this.Background = [System.Windows.Media.Brushes]::Transparent })
        $b.Add_MouseLeftButtonDown({ $_.Handled = $true })
        $b.Add_MouseLeftButtonUp({ $pw = [System.Windows.Window]::GetWindow($this); if ($this.Tag -eq 'min') { $pw.WindowState = 'Minimized' } else { $pw.Close() } })
        [void]$bar.Children.Add($b) }
      [void]$g.Children.Add($bar); $Window.Content = $g
      $Window.Add_MouseLeftButtonDown({ try { $this.DragMove() } catch { } })
      $h = (New-Object System.Windows.Interop.WindowInteropHelper $Window).EnsureHandle()
      $v = 1; [void][PswDwm.Native]::DwmSetWindowAttribute($h, 20, [ref]$v, 4)            # dark frame
      $v = 2; [void][PswDwm.Native]::DwmSetWindowAttribute($h, 33, [ref]$v, 4)            # Windows 11: rounded corners (with its shadow)
      $v = 0x00383838; [void][PswDwm.Native]::DwmSetWindowAttribute($h, 34, [ref]$v, 4)   # Windows 11: thin grey edge
    } catch { }
    $Window.Add_ContentRendered({
      try { $h = (New-Object System.Windows.Interop.WindowInteropHelper $this).Handle
            [PswDwm.Native]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [PswDwm.Native]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
            [void][PswDwm.Native]::ShowWindow($h, 9); [void][PswDwm.Native]::SetForegroundWindow($h); [void][PswDwm.Native]::BringWindowToTop($h) } catch { }
      $this.Activate() | Out-Null })
  }
  $issues = 'https://github.com/mr-Native/PSWloader/issues/new'
  $root = [Windows.Markup.XamlReader]::Parse(@"
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Background="#1B1B1B" Width="800" TextOptions.TextFormattingMode="Display" UseLayoutRounding="True" SnapsToDevicePixels="True">
  <Grid.Resources>
    <Style x:Key="Btn" TargetType="Button">
      <Setter Property="FontFamily" Value="Segoe UI Semibold"/><Setter Property="FontSize" Value="14"/>
      <Setter Property="Height" Value="40"/><Setter Property="Cursor" Value="Hand"/><Setter Property="Padding" Value="22,0"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="B" CornerRadius="8" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                    BorderThickness="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="B" Property="Opacity" Value="0.88"/></Trigger>
              <Trigger Property="IsPressed" Value="True"><Setter TargetName="B" Property="Opacity" Value="0.75"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter TargetName="B" Property="Opacity" Value="0.45"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
  </Grid.Resources>
  <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
  <StackPanel Grid.Row="0" Margin="36,30,36,24">
    <StackPanel Orientation="Horizontal" Margin="0,0,0,18"><Grid Width="30" Height="30"><Ellipse Fill="#3A86E0"/><Path Data="M10,9 L15.5,15 L10,21 M16,9 L21.5,15 L16,21" Stroke="#1B1B1B" StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round"/></Grid><TextBlock Margin="10,0,0,0" VerticalAlignment="Center" FontFamily="Segoe UI" FontWeight="Bold" FontSize="20"><Run Text="PSW" Foreground="#F2F2F0"/><Run Text="loader" Foreground="#3A86E0"/></TextBlock></StackPanel>
    <TextBlock x:Name="Head" Text="Checking PSWloader" FontFamily="Segoe UI Semibold" FontSize="24" Foreground="#F2F2F0" Margin="0,0,0,6"/>
    <TextBlock x:Name="Sub" Text="PSWloader checks the drivers and settings and collects the logs into a support zip. This takes one to two minutes." FontFamily="Segoe UI" FontSize="14" Foreground="#B4B4AE" Margin="0,0,0,20" TextWrapping="Wrap"/>
    <Grid Margin="0,0,0,18">
      <Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="64"/></Grid.ColumnDefinitions>
      <Border Height="12" CornerRadius="6" Background="#2E2E2E" VerticalAlignment="Center"><Grid x:Name="Track" ClipToBounds="True"><Border x:Name="Bar" HorizontalAlignment="Left" Width="0" CornerRadius="6" Background="#3A86E0"/></Grid></Border>
      <TextBlock x:Name="Pct" Grid.Column="1" Text="0 %" FontFamily="Segoe UI Semibold" FontSize="16" Foreground="#F2F2F0" HorizontalAlignment="Right" VerticalAlignment="Center"/>
    </Grid>
    <Border x:Name="List" Background="#242424" BorderBrush="#333333" BorderThickness="1" CornerRadius="10" Padding="16,12" Height="210">
      <ScrollViewer x:Name="Scroll" VerticalScrollBarVisibility="Hidden"><StackPanel x:Name="Steps"/></ScrollViewer>
    </Border>
  </StackPanel>
  <Border Grid.Row="1" Background="#151515" BorderBrush="#2C2C2C" BorderThickness="0,1,0,0" Padding="36,18,36,20">
    <StackPanel Orientation="Horizontal">
      <Button x:Name="Folder" Style="{StaticResource Btn}" Content="Open folder" Background="#3A86E0" Foreground="#FFFFFF" BorderBrush="#3A86E0" BorderThickness="1" IsEnabled="False"/>
      <Button x:Name="Issue" Style="{StaticResource Btn}" Content="Open an issue on GitHub" Background="#262626" Foreground="#F2F2F0" BorderBrush="#454545" BorderThickness="1" Margin="12,0,0,0" IsEnabled="False"/>
      <Button x:Name="Close" Style="{StaticResource Btn}" Content="Close" Background="#262626" Foreground="#F2F2F0" BorderBrush="#454545" BorderThickness="1" Margin="12,0,0,0" IsCancel="True" MinWidth="110"/>
    </StackPanel>
  </Border>
</Grid>
"@)
  $f = @{}; foreach ($n in 'Head','Sub','Track','Bar','Pct','Scroll','Steps','Folder','Issue','Close','List') { $f[$n] = $root.FindName($n) }
  $script:zip = ''; $script:shown = 0; $script:pct = 0; $script:checks = @()
  function New-Line([string]$Text) {
    $g = New-Object System.Windows.Controls.Grid; $g.Margin = '0,3,0,3'
    $c = New-Object System.Windows.Controls.ColumnDefinition; $c.Width = '28'; $g.ColumnDefinitions.Add($c); $g.ColumnDefinitions.Add((New-Object System.Windows.Controls.ColumnDefinition))
    $i = New-Object System.Windows.Controls.TextBlock; $i.FontFamily = 'Segoe MDL2 Assets'; $i.FontSize = 13; $i.VerticalAlignment = 'Center'
    $t = New-Object System.Windows.Controls.TextBlock; $t.Text = $Text; $t.FontSize = 14; $t.VerticalAlignment = 'Center'; [System.Windows.Controls.Grid]::SetColumn($t, 1)
    [void]$g.Children.Add($i); [void]$g.Children.Add($t); $g
  }
  function Set-Line($g, [int]$State) {   # 0 running, 1 done
    $i = $g.Children[0]; $t = $g.Children[1]
    if ($State -eq 0) { $i.Text = [char]0xE91F; $i.Foreground = '#3A86E0'; $t.FontFamily = 'Segoe UI Semibold'; $t.Foreground = '#F2F2F0' }
    else { $i.Text = [char]0xE73E; $i.Foreground = '#7BC67E'; $t.FontFamily = 'Segoe UI'; $t.Foreground = '#B4B4AE' }
  }
  function Set-Pct([int]$p) { $script:pct = $p; $f.Pct.Text = "$p %"; $w = $f.Track.ActualWidth; if ($w -le 0) { $w = 664 }; $f.Bar.Width = [Math]::Max(0, $w * $p / 100) }
  function Add-Lines([string[]]$Lines) {
    foreach ($ln in $Lines) {
      $p = $ln -split "`t", 2; if ($p.Count -lt 2) { continue }
      if ($p[0] -eq 'ZIP') { $script:zip = $p[1]; continue }
      if ($p[0] -eq 'CHECK') { $q = $ln -split "`t", 4; if ($q.Count -ge 4) { $script:checks += ,@([int]$q[1], $q[2], $q[3]) }; continue }
      if ($f.Steps.Children.Count) { Set-Line $f.Steps.Children[$f.Steps.Children.Count - 1] 1 }
      $g = New-Line $p[1]; Set-Line $g $(if ([int]$p[0] -ge 100) { 1 } else { 0 }); [void]$f.Steps.Children.Add($g); Set-Pct ([int]$p[0])
    }
    $f.Scroll.ScrollToEnd()
  }
  function Show-Checks {   # the results replace the step list: icon per state, name, explanation
    $f.Steps.Children.Clear(); $f.List.Height = [double]::NaN; $f.List.MaxHeight = 440   # results: as tall as needed, up to 440
    foreach ($c in ($script:checks | Sort-Object { - $_[0] })) {
      $g = New-Object System.Windows.Controls.Grid; $g.Margin = '0,4,0,4'
      foreach ($w in '28', '190', '*') { $cd = New-Object System.Windows.Controls.ColumnDefinition; $cd.Width = $w; $g.ColumnDefinitions.Add($cd) }
      $i = New-Object System.Windows.Controls.TextBlock; $i.FontFamily = 'Segoe MDL2 Assets'; $i.FontSize = 13; $i.Margin = '0,3,0,0'
      $i.Text = @([char]0xE73E, [char]0xE7BA, [char]0xE711)[$c[0]]; $i.Foreground = @('#7BC67E', '#E8A33D', '#E5534B')[$c[0]]
      $n = New-Object System.Windows.Controls.TextBlock; $n.Text = $c[1]; $n.FontFamily = 'Segoe UI Semibold'; $n.FontSize = 14; $n.Foreground = '#F2F2F0'; [System.Windows.Controls.Grid]::SetColumn($n, 1)
      $d = New-Object System.Windows.Controls.TextBlock; $d.Text = $c[2]; $d.FontSize = 13.5; $d.TextWrapping = 'Wrap'; $d.Foreground = $(if ($c[0]) { '#DADAD5' } else { '#B4B4AE' }); [System.Windows.Controls.Grid]::SetColumn($d, 2)
      [void]$g.Children.Add($i); [void]$g.Children.Add($n); [void]$g.Children.Add($d); [void]$f.Steps.Children.Add($g)
    }
    $f.Scroll.ScrollToHome()
  }
  function Set-Done {
    if ($script:zip -and (Test-Path -LiteralPath $script:zip)) {
      $mb = [math]::Round((Get-Item -LiteralPath $script:zip).Length / 1MB, 1)
      $np = @($script:checks | Where-Object { $_[0] -eq 2 }).Count; $nn = @($script:checks | Where-Object { $_[0] -eq 1 }).Count
      $f.Head.Text = if ($np) { "PSWloader check: $np problem$(if ($np -gt 1) { 's' }) found" } elseif ($nn) { "PSWloader check: $nn note$(if ($nn -gt 1) { 's' })" } else { 'PSWloader check: no problems found' }
      $f.Sub.Text = "Support zip for an issue on GitHub:`n$($script:zip) ($mb MB)`nPersonal details (user and computer names, Wi-Fi names, MAC addresses, serial numbers) were removed from the files."
      if ($script:checks.Count) { Show-Checks }
      $f.Folder.IsEnabled = $true; $f.Issue.IsEnabled = $true; Set-Pct 100
    } else {
      $f.Head.Text = 'The support zip could not be created'
      $f.Sub.Text = 'Run C:\PSWloader\selfcheck\PSWSelfCheck.cmd as administrator to see the details.'
    }
  }
  if ($Preview) {
    Add-Lines @("5`tCollecting system information", "12`tCollecting PSWloader files and logs", "20`tReading devices and drivers", "30`tReading registry settings",
                "38`tReading boot configuration, disks and network", "46`tReading firmware logs", "55`tCollecting event logs", "68`tCollecting setup logs and crash dumps",
                "76`tReading scheduled tasks and processes", "82`tCreating the graphics report", "94`tCreating the zip file", "100`tDone")
    $script:checks = @(@(0, 'PSWloader', 'Version v1.2 is installed.'), @(0, 'Firmware', 'PSWloader v1.2 (r5.28.14)'), @(0, 'Test signing', 'On (needed for the PS5 drivers).'),
      @(0, 'Graphics driver', 'Running, version 0.7.216.1230.'), @(2, 'Wi-Fi driver', 'The device reports a problem (code 10). Restart Windows; if it stays, attach the zip to an issue.'),
      @(0, 'Bluetooth driver', 'Running, version 0.7.0.0.'), @(1, 'Crashes', 'Windows stopped with an error once in the last 7 days (the reports are in the zip).'), @(0, 'Free space', '182.4 GB free on C:.'))
    $f.Head.Text = 'PSWloader check: 1 problem found'; $f.Sub.Text = "Support zip for an issue on GitHub:`nC:\PSWloader\selfcheck\results\PSW-selfcheck-2026-11-26_15-00-00.zip (12.4 MB)`nPersonal details (user and computer names, Wi-Fi names, MAC addresses, serial numbers) were removed from the files."
    Show-Checks; $f.Folder.IsEnabled = $true; $f.Issue.IsEnabled = $true
    $root.Measure([System.Windows.Size]::new([double]::PositiveInfinity, [double]::PositiveInfinity)); $root.Arrange([System.Windows.Rect]::new($root.DesiredSize)); $root.UpdateLayout(); Set-Pct 100; $root.UpdateLayout()
    $bmp = [System.Windows.Media.Imaging.RenderTargetBitmap]::new([int]($root.ActualWidth * 2), [int]($root.ActualHeight * 2), 192, 192, [System.Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($root); $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder; $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $fs = [IO.File]::Create($Preview); $enc.Save($fs); $fs.Close(); exit 0
  }
  $pf = Join-Path $env:TEMP "psw-selfcheck-progress-$PID.txt"; Set-Content -LiteralPath $pf -Value '' -NoNewline
  $child = Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -NoExplorer -ProgressFile `"$pf`""
  $w = New-Object System.Windows.Window
  $w.Title = 'PSWloader Support'; $w.Content = $root; $w.SizeToContent = 'WidthAndHeight'; $w.ResizeMode = 'CanMinimize'; $w.WindowStartupLocation = 'CenterScreen'
  $f.Folder.Add_Click({ Start-Process explorer.exe "/select,`"$($script:zip)`"" })
  $f.Issue.Add_Click({ Start-Process $issues })
  $f.Close.Add_Click({ $w.Close() })
  $timer = New-Object System.Windows.Threading.DispatcherTimer; $timer.Interval = [TimeSpan]::FromMilliseconds(300)
  $timer.Add_Tick({
    try { $fs = [IO.File]::Open($pf, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
          $all = @(($sr.ReadToEnd() -split "`r?`n") | Where-Object { $_ -ne '' }); $sr.Close()
          if ($all.Count -gt $script:shown) { Add-Lines $all[$script:shown..($all.Count - 1)]; $script:shown = $all.Count } } catch { }
    if ($child.HasExited) { $timer.Stop(); Set-Done; Remove-Item -LiteralPath $pf -Force -ErrorAction SilentlyContinue }
  })
  $w.Add_ContentRendered({ Set-Pct 0; $timer.Start() })
  Set-PswDarkTitle $w; [void]$w.ShowDialog()
  exit 0
}
$ErrorActionPreference = 'Continue'
$Ver = '1.2'
$Stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$Base = 'C:\PSWloader\selfcheck\results'
$Work = Join-Path $Base "PSW-selfcheck-$Stamp"
if ($ChecksOnly) { $Work = Join-Path $env:TEMP "psw-checks-$PID" }   # v1.7: checks only (Dashboard) - no zip, work folder removed again
$Zip = "$Work.zip"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Log = Join-Path $Work 'selfcheck.log'
function L([string]$m) { $l = '{0:HH:mm:ss} {1}' -f (Get-Date), $m; Add-Content -Path $Log -Value $l; Write-Host $l }
function Out-Txt([string]$Name, $Data) { $Data | Out-String -Width 400 | Set-Content -Path (Join-Path $Work $Name) -Encoding UTF8 }
function Step([string]$Name, [scriptblock]$Do) {
  try { & $Do; L "ok    $Name" } catch { L "error $Name : $($_.Exception.Message)" }
}
function Copy-IfThere([string]$Src, [string]$DstDir) {
  if (Test-Path -LiteralPath $Src) {
    New-Item -ItemType Directory -Force -Path $DstDir | Out-Null
    Copy-Item -LiteralPath $Src -Destination $DstDir -Recurse -Force -ErrorAction SilentlyContinue
  }
}
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
L "PSWSelfCheck $Ver  admin=$admin  output=$Zip"

Prog 3 'Checking PSWloader'
# 0. v1.5 (user 26.11.2026): PSWloader check - state 0 ok, 1 note, 2 problem; checks.txt in the zip, CHECK lines for the window
$Checks = New-Object System.Collections.Generic.List[object]
function Chk([int]$State, [string]$Name, [string]$Detail, [string]$Fix = '') {   # v1.7: $Fix = what Dashboard troubleshooting can do
  $Checks.Add([pscustomobject]@{ State = $State; Name = $Name; Detail = $Detail; Fix = $Fix })
  if ($ProgressFile) { try { Add-Content -LiteralPath $ProgressFile -Value "CHECK`t$State`t$Name`t$Detail" -Encoding UTF8 } catch { } }
}
Step 'pswloader check' {
  $vj = $null; if (Test-Path 'C:\ProgramData\PSWloader\version.json') { $vj = Get-Content 'C:\ProgramData\PSWloader\version.json' -Raw | ConvertFrom-Json }
  if ($vj) { Chk 0 'PSWloader' "Version $($vj.version) is installed." } else { Chk 1 'PSWloader' 'No version information found (C:\ProgramData\PSWloader\version.json).' }
  # console identity (PS5 Linux board names, from the board id ps5pwr reads from the EMC); a PS5 Pro has the Viola GPU 13DA
  $pp = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters' -ErrorAction SilentlyContinue
  $script:Console = [ordered]@{ model = 'PlayStation 5'; chassis = ''; board = 'unknown'; boardId = ''; gddr6 = ''; gpu = '' }
  $gp = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_(13[0-9A-F]{2})' } | Select-Object -First 1
  if ($gp -and $gp.InstanceId -match 'DEV_(13[0-9A-F]{2})') { $script:Console.gpu = $Matches[1]; if ($Matches[1] -eq '13DA') { $script:Console.model = 'PlayStation 5 (Pro)' } }
  if ($pp -and $pp.FanBoardId) {
    $raw = [uint64]$pp.FanBoardId; $be = [uint64]0; for ($i = 0; $i -lt 8; $i++) { $be = ($be -shl 8) -bor (($raw -shr (8 * $i)) -band 0xFF) }
    $a = $be -band [uint64]'0xFFFFFFFFFFFFFF00'; $b = $be -band [uint64]'0xFFFFFFFFFF00FF00'; $v = [int]$pp.FanGddr6Id -band 0xF
    $script:Console.boardId = '{0:x16}' -f $raw; $script:Console.gddr6 = '{0:x4}' -f [int]$pp.FanGddr6Id
    # v1.8 (user 26.11.2026: "PlayStation 5 (Slim / Fat / Pro)"): the third byte of a retail board id (30 02 xx ...) is the chassis
    # generation - 1 = A, 2 = B, 3 = C (the three original "Fat" generations), 4 = D and later = Slim (PS5 Linux names its
    # tables A-/B-/D-Eiger/Denali the same way); the Pro is known by its GPU (Viola, 13DA)
    $gen = [int](($be -shr 40) -band 0xFF)
    if ((($be -shr 48) -band 0xFFFF) -eq 0x3002 -and $gen -ge 1 -and $gen -le 9) {
      $script:Console.chassis = [string][char](64 + $gen)
      if ($script:Console.gpu -ne '13DA') { $script:Console.model = if ($gen -le 3) { 'PlayStation 5 (Fat)' } else { 'PlayStation 5 (Slim)' } } }
    $mem = @{ 1 = 'Samsung'; 6 = 'Hynix'; 15 = 'Micron' }[$v]
    $script:Console.board = switch ($true) {
      ($a -in [uint64]'0x2001010104010400', [uint64]'0x2001010104010500') { 'Sancarlo'; break }
      ($a -in [uint64]'0x3002010104010400', [uint64]'0x3002010104010500') { "A-Eiger $mem"; break }
      ($a -in [uint64]'0x3002010104030400', [uint64]'0x3002010104030500') { "A-Denali $mem"; break }
      ($b -in [uint64]'0x3002020101000400', [uint64]'0x3002020101000500') { "B-Eiger/Denali $mem"; break }
      ($b -in [uint64]'0x3002040101000400', [uint64]'0x3002040101000500') { "D-Eiger/Denali $mem"; break }
      default { 'unknown board' } }
  }
  $bn = if ($script:Console.board -eq 'unknown board') { "board not in the PS5 Linux list$(if ($script:Console.boardId) { " (id $($script:Console.boardId), GDDR6 $($script:Console.gddr6))" })" } else { "board $($script:Console.board)" }
  Chk 0 'Console' "$($script:Console.model)$(if ($script:Console.chassis) { ", chassis $($script:Console.chassis)" }), $bn$(if ($script:Console.gpu) { ", GPU $($script:Console.gpu)" })."
  # v1.6 (user 26.11.2026): restart pending - an update/setup newer than the last start, or Windows itself waits for a restart;
  # older drivers or firmware are then "restart to load", not "run the updater"
  $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime; $since = $null
  try { if ($vj -and $vj.updatedAt) { $since = [datetime]$vj.updatedAt } } catch { }
  $sr = Get-Item "$env:ProgramData\PSWloader\setup-result.json" -ErrorAction SilentlyContinue; if ($sr -and (-not $since -or $sr.LastWriteTime -gt $since)) { $since = $sr.LastWriteTime }
  $winPending = (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') -or
                (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') -or
                [bool](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue)
  $script:RestartPending = ($since -and $since -gt $boot) -or $winPending
  if ($since -and $since -gt $boot) { Chk 1 'Restart' "PSWloader $(if ($vj) { $vj.version }) was installed on $($since.ToString('yyyy-MM-dd HH:mm')) - restart Windows to finish it (new drivers and firmware load then)." 'restart' }
  elseif ($winPending) { Chk 1 'Restart' 'Windows waits for a restart (updates or drivers).' 'restart' }
  # v1.7: hardware fingerprint - this Windows on another console (board, GPU) or with another internal SSD; the first check
  # with rights to write keeps the fingerprint, troubleshooting sets the console-specific parts up again and takes the new one
  $nvm = (Get-PhysicalDisk -ErrorAction SilentlyContinue | Where-Object { $_.BusType -eq 'NVMe' } | Select-Object -First 1).FriendlyName
  $fp = [ordered]@{ board = $script:Console.boardId; gddr6 = $script:Console.gddr6; gpu = $script:Console.gpu; nvme = [string]$nvm }
  $hwf = "$env:ProgramData\PSWloader\dashboard\hardware.json"; $old = $null; New-Item -ItemType Directory -Force (Split-Path $hwf) -ErrorAction SilentlyContinue | Out-Null; try { $old = Get-Content $hwf -Raw | ConvertFrom-Json } catch { }
  if (-not $old) { try { $fp | ConvertTo-Json | Set-Content -LiteralPath $hwf -Encoding UTF8 -ErrorAction Stop } catch { } }
  else {
    $diff = @(foreach ($k in 'board', 'gpu', 'nvme') { if ([string]$old.$k -and [string]$fp[$k] -and [string]$old.$k -ne [string]$fp[$k]) { "$k $($old.$k) -> $($fp[$k])" } })
    if ($diff.Count) { Chk 2 'Hardware' ("This Windows was set up on other hardware (" + ($diff -join ', ') + '). Troubleshooting sets PSWloader up for this console.') 'hardware' }
    else { Chk 0 'Hardware' 'Same console and SSD as at the installation.' }
  }
  # firmware / PSWloader.elf: BIOS version "PSWloader v1.2 (r5.28.14)"
  $bios = [string](Get-CimInstance Win32_BIOS).SMBIOSBIOSVersion
  $rev = { param($t) if ($t -match 'r(\d+)\.(\d+)\.(\d+)') { [int]$Matches[1] * 1000000 + [int]$Matches[2] * 1000 + [int]$Matches[3] } }
  $need = if ($vj -and $vj.minFirmware) { & $rev ([string]$vj.minFirmware) } else { $null }; $have = & $rev $bios
  if ($bios -notlike 'PSWloader*') { Chk 2 'Firmware' "Windows was not started by PSWloader (BIOS: $bios)." }
  elseif ($need -and (-not $have -or $have -lt $need)) { Chk 2 'Firmware' "PSWloader.elf is outdated ($bios). Replace it in the Payload Manager with the one from the $($vj.version) release." }
  elseif ($have -and $have -lt (& $rev 'r5.28.14')) {
    if ($script:RestartPending) { Chk 1 'Firmware' "$bios still runs - the update put r5.28.14 on the EFI partition, it starts after the restart." }
    else { Chk 1 'Firmware' "$bios - older than in PSWloader v1.2 (r5.28.14). Run the updater, and replace PSWloader.elf if it stays old after a restart." } }
  else { Chk 0 'Firmware' "$bios" }
  $ts = ((bcdedit /enum '{current}' 2>$null | Select-String -Pattern 'testsigning') -join ' ')
  if ($ts -match 'Yes') { Chk 0 'Test signing' 'On (needed for the PS5 drivers).' } else { Chk 2 'Test signing' 'Off - the PS5 drivers cannot load. Troubleshooting turns it on (restart needed).' 'testsigning' }
  # PS5 drivers by service; the versions of this release (PSWloader v1.2) - an older one is a note (run the updater)
  $expect = @{ bc250kmd = '0.7.216.1230'; ps5pwr = '1.19.2.0'; ps5gbe = '1.0.10.1'; ps5wifi = '1.0.7.0'; ps5btx = '0.7.0.0' }
  foreach ($d in @(@('bc250kmd', 'Graphics driver'), @('ps5pwr', 'Power and fan driver'), @('ps5gbe', 'Ethernet driver'), @('ps5wifi', 'Wi-Fi driver'), @('ps5btx', 'Bluetooth driver'))) {
    $e = @(Get-CimInstance Win32_PnPEntity -Filter "Service='$($d[0])'" -ErrorAction SilentlyContinue)
    if (-not $e.Count) { Chk 2 $d[1] 'Not installed or the device is not found.' "driver:$($d[0])"; continue }
    $err = @($e | Where-Object { $_.ConfigManagerErrorCode -ne 0 })
    $ver = (Get-PnpDeviceProperty -InstanceId $e[0].PNPDeviceID -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    if ($d[0] -eq 'ps5pwr') {   # v1.9: an update points the service at the new package (no device reinstall) - its INF tells the version
      $ip = [string](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5pwr' -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
      $pinf = Join-Path (Split-Path -Parent ($ip -replace '^\\SystemRoot', $env:SystemRoot)) 'ps5pwr.inf'
      $m = if (Test-Path -LiteralPath $pinf) { Select-String -LiteralPath $pinf -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*([0-9.]+)' | Select-Object -First 1 } else { $null }
      if ($m) { $ver = $m.Matches[0].Groups[1].Value } }
    if ($err.Count) { Chk 2 $d[1] "The device reports a problem (code $($err[0].ConfigManagerErrorCode)). Troubleshooting repairs the driver and restarts the device." "device:$($d[0])" }
    elseif ($ver -and $expect[$d[0]] -and ([version]$ver -lt [version]$expect[$d[0]])) {
      if ($script:RestartPending) { Chk 1 $d[1] "Version $ver still loaded - $($expect[$d[0]]) is installed and loads after the restart." }
      else { Chk 1 $d[1] "Running, version $ver - older than in PSWloader v1.2 ($($expect[$d[0]])). Run the updater." } }
    else { Chk 0 $d[1] "Running$(if ($ver) { ", version $ver" })." }
  }
  # v1.8 (user 26.11.2026: problems inside a driver that looks installed): a GPU driver that is installed but not used,
  # a GPU driver without its state, Wi-Fi that runs but finds no networks
  $vc = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -match 'VEN_1002&DEV_13' })
  if ($vc.Count -and $vc[0].Name -match 'Basic') { Chk 2 'Graphics' 'Windows uses its basic display driver instead of the PS5 graphics driver. Troubleshooting installs the PS5 driver again.' 'driver:bc250kmd' }
  $gp = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd'
  if ((Test-Path "$gp\Parameters") -and -not (Test-Path "$gp\SharedState\StateV2") -and @(Get-CimInstance Win32_PnPEntity -Filter "Service='bc250kmd'" -ErrorAction SilentlyContinue | Where-Object { $_.ConfigManagerErrorCode -ne 0 }).Count) {
    Chk 2 'Graphics settings' 'The graphics driver could not create its settings at its first start. Troubleshooting sets them up and restarts the device.' 'device:bc250kmd' }
  $wp = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\ps5wifi\Parameters' -ErrorAction SilentlyContinue
  # the driver records how far the chip start got (Stage 10 = ready); HW 26.11.: Stage 5 = the chip did not answer after the
  # firmware download, Windows still lists the adapter as OK and finds no networks
  if ($wp -and $null -ne $wp.Stage -and [int]$wp.Stage -ne 10) {
    Chk 2 'Wi-Fi' ("The Wi-Fi chip did not start (step $($wp.Stage), code 0x{0:X}). Switch the console off completely and start PSWloader again from GameOS; troubleshooting restarts the adapter." -f [int]$wp.LastErr) 'device:ps5wifi' }
  elseif ($wp -and [int]$wp.ScanCount -ge 10 -and [int]$wp.BssCount -eq 0) { Chk 1 'Wi-Fi' "The Wi-Fi driver runs but has found no networks in $($wp.ScanCount) scans. Troubleshooting restarts the Wi-Fi adapter." 'device:ps5wifi' }
  # internal M.2: the PS5 NVMe filter on every NVMe in the slot
  $nv = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\*' -and @($_.CompatibleID) -contains 'PCI\CC_010802' })
  foreach ($n in $nv) {
    $lf = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($n.InstanceId)" -Name LowerFilters -ErrorAction SilentlyContinue).LowerFilters
    if ($n.Status -ne 'OK') { Chk 2 'Internal M.2 SSD' "The SSD reports a problem ($($n.Status))." }
    elseif (@($lf) -notcontains 'ps5nvmsi') { Chk 1 'Internal M.2 SSD' 'The PS5 NVMe filter is missing; PSWloader Watch adds it at the next start.' 'm2filter' }
    else { $dk = Get-PhysicalDisk -ErrorAction SilentlyContinue | Where-Object { $_.BusType -eq 'NVMe' } | Select-Object -First 1
           Chk 0 'Internal M.2 SSD' "$(if ($dk) { "$($dk.FriendlyName), $([math]::Round($dk.Size / 1GB)) GB" } else { $n.FriendlyName }) - ready." }
  }
  # v1.9 (HW 26.11.2026): USB disks run Bulk-Only - with UAS a USB SSD at 10 Gbit/s crashed the PS5 USB stack (0x144) or stopped
  # with 0x7B. PSWloader Watch points the UAS driver service at pswbot.sys (a copy of USBSTOR.SYS); it takes effect at the next start.
  $uip = [string](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\UASPStor' -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
  $uld = [string](Get-CimInstance Win32_SystemDriver -Filter "Name='UASPStor'" -ErrorAction SilentlyContinue).PathName
  if ($uip -and $uip -notmatch '(?i)\\pswbot\.sys$') { Chk 1 'USB disk mode' 'USB disks use UAS, which can crash the PS5 USB stack. Troubleshooting switches them to the stable Bulk-Only mode (restart needed).' 'usbbot' }
  elseif ($uld -and $uld -notmatch '(?i)\\pswbot\.sys$' -and (Get-CimInstance Win32_SystemDriver -Filter "Name='UASPStor'").State -eq 'Running') { Chk 1 'USB disk mode' 'Bulk-Only is set and takes effect after the next restart.' }
  elseif ($uip) { Chk 0 'USB disk mode' 'USB disks run Bulk-Only (the stable mode).' }
  # code 28 = no driver: about 20 Sony PCI functions of the console have no Windows driver - expected, not counted
  $bad = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.ConfigManagerErrorCode -notin 0, 28 })
  if ($bad.Count) { Chk 1 'Other devices' ("$($bad.Count) device(s) with a problem: " + (($bad | Select-Object -First 3 | ForEach-Object { "$($_.Name) (code $($_.ConfigManagerErrorCode))" }) -join ', ') + '.') } else { Chk 0 'Other devices' 'No device reports a problem.' }
  $t = Get-ScheduledTask -TaskName 'PSWloader Watch' -ErrorAction SilentlyContinue
  if ($t -and $t.State -ne 'Disabled') { Chk 0 'PSWloader Watch' 'The background check is active.' } else { Chk 1 'PSWloader Watch' 'The background check is not active - troubleshooting turns it on again.' 'watchtask' }
  # v1.8 (user 26.11.2026: crashed tasks and services belong into Health and troubleshooting; HW 26.11.: the ThermalManager had
  # ended with an error and nothing showed it): PSWloader's background tasks. The ThermalManager starts two minutes after the
  # sign-in; a task that ended without an error (result 0) had nothing to do.
  $ex = @(Get-Process explorer -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -First 1)
  $signedInMin = if ($ex.Count) { ((Get-Date) - $ex[0].StartTime).TotalMinutes } else { 0 }
  foreach ($bt in @(@('PSW-ThermalManager', 'Thermal control', $true), @('PSW-ThermalGuard', 'Thermal guard', $false), @('PSW-PowerButton', 'Power button', $false))) {
    $tk = Get-ScheduledTask -TaskName $bt[0] -ErrorAction SilentlyContinue
    if (-not $tk) { Chk 1 $bt[1] "The task $($bt[0]) is missing - run the PSWloader updater."; continue }
    $ti = $tk | Get-ScheduledTaskInfo -ErrorAction SilentlyContinue; $res = [int64]$ti.LastTaskResult
    if ($tk.State -eq 'Running') { Chk 0 $bt[1] 'Running.' }
    elseif ($tk.State -eq 'Disabled') { Chk 2 $bt[1] "The task $($bt[0]) is turned off. Troubleshooting turns it on and starts it." "task:$($bt[0])" }
    elseif ($bt[2] -and $signedInMin -lt 5) { Chk 0 $bt[1] 'Starts two minutes after the sign-in.' }
    elseif ($res -eq 0) { Chk 0 $bt[1] 'Not running - it ended without an error.' }
    else { Chk 2 $bt[1] ("Not running - the task $($bt[0]) stopped with error 0x{0:X}" -f $res + $(if ($ti.LastRunTime) { " (started $($ti.LastRunTime.ToString('yyyy-MM-dd HH:mm')))" }) + '. Troubleshooting starts it again.') "task:$($bt[0])" }
  }
  # services of PSWloader that should run, and services Windows saw stopping unexpectedly in the last 24 hours
  foreach ($sv in @(Get-CimInstance Win32_Service -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(psw|ps5|bc250)' -and $_.StartMode -eq 'Auto' -and $_.State -ne 'Running' })) {
    Chk 2 "Service $($sv.Name)" "The service is not running (state $($sv.State), exit code $($sv.ExitCode)). Troubleshooting starts it." "service:$($sv.Name)" }
  $crashed = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Service Control Manager'; Id = 7031, 7034; StartTime = (Get-Date).AddDays(-1) } -ErrorAction SilentlyContinue)
  if ($crashed.Count) {
    $names = @($crashed | ForEach-Object { [string]$_.Properties[0].Value } | Group-Object | Sort-Object Count -Descending | Select-Object -First 3 | ForEach-Object { "$($_.Name) ($($_.Count)x)" })
    Chk 1 'Services' "Windows services stopped unexpectedly in the last 24 hours: $($names -join ', '). The support zip has the details." }
  else { Chk 0 'Services' 'No service stopped unexpectedly in the last 24 hours.' }
  $c = Get-PSDrive C -ErrorAction SilentlyContinue; $gb = if ($c) { [math]::Round($c.Free / 1GB, 1) } else { 0 }
  if ($gb -lt 3) { Chk 2 'Free space' "Only $gb GB free on C:. Windows needs more room to run and update." } elseif ($gb -lt 10) { Chk 1 'Free space' "$gb GB free on C: - getting low." } else { Chk 0 'Free space' "$gb GB free on C:." }
  $dumps = @(Get-ChildItem "$env:SystemRoot\Minidump" -Filter *.dmp -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt (Get-Date).AddDays(-7) })
  if ($dumps.Count) { Chk 1 'Crashes' "Windows stopped with an error $(if ($dumps.Count -eq 1) { 'once' } else { "$($dumps.Count) times" }) in the last 7 days (the reports are in the zip)." } else { Chk 0 'Crashes' 'No crash in the last 7 days.' }
  $up = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' })
  if ($up.Count) { Chk 0 'Network' "Connected ($(($up | ForEach-Object { $_.Name }) -join ', '))." } else { Chk 1 'Network' 'No network connection.' }
  ($Checks | ForEach-Object { '{0,-8} {1,-22} {2}' -f @('ok', 'note', 'PROBLEM')[$_.State], $_.Name, $_.Detail }) | Set-Content (Join-Path $Work 'checks.txt') -Encoding UTF8
}

# v1.7 (PSWloader Dashboard): -ChecksOnly writes the results as JSON (default C:\ProgramData\PSWloader\dashboard\checks.json) and stops
if ($ChecksOnly) {
  $out = if ($ResultFile) { $ResultFile } else { "$env:ProgramData\PSWloader\dashboard\checks.json" }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
  $json = [ordered]@{ schema = 1; time = (Get-Date).ToString('s'); restartPending = [bool]$script:RestartPending; console = $script:Console
    checks = @($Checks | ForEach-Object { [ordered]@{ state = $_.State; name = $_.Name; detail = $_.Detail; fix = $_.Fix } }) } | ConvertTo-Json -Depth 5
  Set-Content -LiteralPath "$out.tmp" -Value $json -Encoding UTF8; Move-Item -LiteralPath "$out.tmp" -Destination $out -Force
  Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
  exit 0
}

Prog 5 'Collecting system information'
# 1. summary
Step 'summary' {
  $os = Get-CimInstance Win32_OperatingSystem
  $cs = Get-CimInstance Win32_ComputerSystem
  $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
  $vj = $null; if (Test-Path 'C:\ProgramData\PSWloader\version.json') { $vj = Get-Content 'C:\ProgramData\PSWloader\version.json' -Raw | ConvertFrom-Json }
  $gpu = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13(FC|DB|DC|DA|F9|FA|FB|FE)' } | Select-Object -First 1
  $pwr = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_104D&DEV_9107' } | Select-Object -First 1
  $drv = { param($d) if ($d) { ($d | Get-PnpDeviceProperty DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data } }
  $bad = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.Status -ne 'OK' })
  $s = [ordered]@{
    'selfcheck version'     = $Ver
    'created'               = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz')
    'computer'              = $env:COMPUTERNAME
    'pswloader version'     = $(if ($vj) { $vj.version } else { 'no version.json' })
    'pswloader updated at'  = $(if ($vj) { $vj.updatedAt } else { '' })
    'pswloader previous'    = $(if ($vj) { $vj.previousVersion } else { '' })
    'windows installed at'  = $os.InstallDate.ToString('yyyy-MM-dd HH:mm:ss')
    'windows'               = "$($os.Caption) $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR) $($os.OSArchitecture)"
    'language'              = (Get-Culture).Name + ' / UI ' + (Get-UICulture).Name
    'last boot'             = $os.LastBootUpTime.ToString('yyyy-MM-dd HH:mm:ss')
    'model'                 = "$($cs.Manufacturer) $($cs.Model)"
    'memory GB'             = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
    'gpu'                   = $(if ($gpu) { "$($gpu.FriendlyName) | $($gpu.Status) $($gpu.Problem) | driver $(& $drv $gpu) | $($gpu.InstanceId)" } else { 'not found' })
    'ps5pwr'                = $(if ($pwr) { "$($pwr.Status) | driver $(& $drv $pwr)" } else { 'not found' })
    'devices not ok'        = $bad.Count
    'testsigning'           = ((bcdedit /enum '{current}' 2>$null | Select-String -Pattern 'testsigning') -join ' ').Trim()
    'memory integrity'      = $(try { $g = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction Stop; "VBS status $($g.VirtualizationBasedSecurityStatus), running $(@($g.SecurityServicesRunning) -join ',') (2 = HVCI on: PS5 drivers cannot load)" } catch { 'unknown' })
  }
  (@($s.Keys) | ForEach-Object { '{0,-22} {1}' -f $_, $s[$_] }) | Set-Content (Join-Path $Work 'summary.txt') -Encoding UTF8
}

Prog 12 'Collecting PSWloader files and logs'
# 2. PSWloader files and logs
Step 'pswloader logs' {
  $d = Join-Path $Work 'pswloader'
  foreach ($p in 'C:\ProgramData\PSWloader\version.json', 'C:\ProgramData\PSWloader\journal.json', 'C:\ProgramData\PSWloader\updater.json',
                 'C:\ProgramData\PSWloader\logs', 'C:\PSWloader\logs\watch', 'C:\PSWloader\logs\ps5btx.log', 'C:\ps5-work\ps5-postboot.log', 'C:\ps5-work\swap-kmd.log') { Copy-IfThere $p $d }
  if (Test-Path 'C:\PSWSetup') { Get-ChildItem 'C:\PSWSetup' -File -Include *.log, *.txt, *.json -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.Length -lt 20MB } | ForEach-Object { Copy-IfThere $_.FullName (Join-Path $d 'PSWSetup') } }
  Get-ChildItem 'C:\ps5-work' -Filter *.log -File -ErrorAction SilentlyContinue | Where-Object { $_.Length -lt 20MB } |
    ForEach-Object { Copy-IfThere $_.FullName (Join-Path $d 'ps5-work') }
}

Prog 20 'Reading devices and drivers'
# 3. devices and drivers
Step 'devices' {
  $all = Get-PnpDevice -ErrorAction SilentlyContinue
  Out-Txt 'devices-all.txt' ($all | Sort-Object Class, FriendlyName | Format-Table Status, Problem, Class, FriendlyName, InstanceId -AutoSize)
  $rows = foreach ($dv in ($all | Where-Object { $_.Present -and ($_.Status -ne 'OK' -or $_.InstanceId -match 'VEN_1002|VEN_104D|VEN_1B4B|VEN_1022|USB\\' -or @($_.CompatibleID) -like 'PCI\CC_0108*') })) {   # v1.4: every NVMe (any vendor)
    $p = $dv | Get-PnpDeviceProperty DEVPKEY_Device_DriverVersion, DEVPKEY_Device_DriverInfPath, DEVPKEY_Device_Service, DEVPKEY_Device_ProblemStatus, DEVPKEY_Device_LocationInfo -ErrorAction SilentlyContinue
    $g = { param($k) ($p | Where-Object KeyName -eq $k).Data }
    [pscustomobject]@{ Status = $dv.Status; Problem = $dv.Problem; ProblemStatus = ('0x{0:X8}' -f [uint32](& $g 'DEVPKEY_Device_ProblemStatus'));
      Name = $dv.FriendlyName; Driver = (& $g 'DEVPKEY_Device_DriverVersion'); Inf = (& $g 'DEVPKEY_Device_DriverInfPath');
      Service = (& $g 'DEVPKEY_Device_Service'); Location = (& $g 'DEVPKEY_Device_LocationInfo'); Id = $dv.InstanceId }
  }
  Out-Txt 'devices-detail.txt' ($rows | Format-List)
  pnputil /enum-drivers > (Join-Path $Work 'drivers-pnputil.txt') 2>&1
  Out-Txt 'drivers-system.txt' (Get-CimInstance Win32_SystemDriver | Sort-Object Name | Format-Table Name, State, StartMode, PathName -AutoSize)
}

Prog 30 'Reading registry settings'
# 4. registry
Step 'registry' {
  $d = Join-Path $Work 'registry'; New-Item -ItemType Directory -Force $d | Out-Null
  foreach ($s in 'bc250kmd', 'ps5pwr', 'ps5gbe', 'ps5wifi', 'ps5nvmsi', 'ps5m2flt', 'ps5cmos', 'pswdisk', 'pswsvc', 'stornvme', 'ps5usb') {
    if (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\$s") { reg export "HKLM\SYSTEM\CurrentControlSet\Services\$s" (Join-Path $d "service-$s.reg") /y | Out-Null }
  }
  foreach ($k in 'HKLM\SOFTWARE\amdgpu-wddm', 'HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers', 'HKLM\SYSTEM\Setup', 'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion') {
    $n = ($k -replace '[\\ ]', '_') + '.reg'; reg export $k (Join-Path $d $n) /y 2>$null | Out-Null
  }
  # v1.4: GPU, Sony, Wi-Fi and every NVMe in the M.2 slot (any vendor - class 01/08/02 in the CompatibleIDs)
  Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\PCI" -ErrorAction SilentlyContinue |
    Where-Object { $_.PSChildName -match '^VEN_(1002|104D|1B4B)&' -or @(Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue | Where-Object { @($_.GetValue('CompatibleIDs')) -like 'PCI\CC_010802*' }).Count } |
    ForEach-Object { reg export ($_.Name) (Join-Path $d ("enum-" + $_.PSChildName.Replace('&', '_') + '.reg')) /y 2>$null | Out-Null }
}

Prog 38 'Reading boot configuration, disks and network'
# 5. boot configuration, disks, network
Step 'boot and disks' {
  bcdedit /enum all /v > (Join-Path $Work 'bcd.txt') 2>&1
  Out-Txt 'disks.txt' @((Get-Disk | Format-List Number, FriendlyName, SerialNumber, BusType, PartitionStyle, Size, HealthStatus, OperationalStatus),
                        (Get-Partition | Format-Table DiskNumber, PartitionNumber, DriveLetter, Type, GptType, Size -AutoSize),
                        (Get-Volume | Format-Table DriveLetter, FileSystemLabel, FileSystem, DriveType, HealthStatus, Size, SizeRemaining -AutoSize))
  Out-Txt 'network.txt' @((Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Format-Table Name, InterfaceDescription, Status, LinkSpeed, MacAddress, DriverVersion -AutoSize),
                          (ipconfig /all))
}

Prog 46 'Reading firmware logs'
# 6. UEFI self check and firmware logs (EFI system partition and USB drives)
Step 'uefi logs' {
  $d = Join-Path $Work 'uefi'; New-Item -ItemType Directory -Force $d | Out-Null
  $names = 'PS5DIAG.LOG', 'PS5UEFI.LOG', 'PS5WARM.LOG', 'PS5RESCU.LOG', 'PS5UEFI.CFG', 'PS5POWER.CFG', 'PSWloader\logs\PS5DIAG.LOG'
  # v1.2: an ESP that already has a drive letter is used as it is (mountvol /s fails then, no logs were collected)
  $letter = $null; $espTemp = $false
  $sp = Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.IsSystem } | Select-Object -First 1
  if ($sp -and $sp.DriveLetter -and $sp.DriveLetter -ne [char]0) { $letter = [string]$sp.DriveLetter }
  else { foreach ($c in [char[]]'SRQPONMLKJIHGT') { if (-not (Test-Path "${c}:\")) { $letter = $c; $espTemp = $true; break } } }
  if ($letter) {
    if ($espTemp) { mountvol "${letter}:" /s 2>$null | Out-Null }
    if (Test-Path "${letter}:\") {
      foreach ($n in $names) { if (Test-Path "${letter}:\$n") { Copy-Item "${letter}:\$n" (Join-Path $d ('esp-' + ($n -replace '\\', '_'))) -Force } }
      Get-ChildItem "${letter}:\" -Recurse -File -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime |
        Format-Table -AutoSize | Out-String -Width 300 | Set-Content (Join-Path $d 'esp-files.txt')
      if ($espTemp) { mountvol "${letter}:" /d 2>$null | Out-Null }
      # firmware log slots are a ring (16-byte file marker, 'PS5LOG1', u64 bytes written, 64 KiB ring): readable copy in order
      foreach ($f in Get-ChildItem $d -Filter 'esp-PS5*.LOG' -File) {
        $b = [IO.File]::ReadAllBytes($f.FullName)
        if ($b.Length -lt 32 + 65536 -or [Text.Encoding]::ASCII.GetString($b, 16, 7) -ne 'PS5LOG1') { continue }
        $tot = [BitConverter]::ToUInt64($b, 24); $r = 65536
        if ($tot -le $r) { $txt = [Text.Encoding]::ASCII.GetString($b, 32, [int]$tot) }
        else { $s = [int]($tot % $r); $txt = [Text.Encoding]::ASCII.GetString($b, 32 + $s, $r - $s) + [Text.Encoding]::ASCII.GetString($b, 32, $s) }
        Set-Content -Path ($f.FullName -replace '\.LOG$', '.txt') -Value $txt -Encoding ASCII
      }
    }
  }
  foreach ($v in (Get-Volume | Where-Object { $_.DriveLetter -and $_.DriveType -eq 'Removable' -or ($_.DriveLetter -and (Get-Partition -DriveLetter $_.DriveLetter -ErrorAction SilentlyContinue | Get-Disk -ErrorAction SilentlyContinue).BusType -eq 'USB') })) {
    foreach ($n in $names) { $p = "$($v.DriveLetter):\$n"; if (Test-Path $p) { Copy-Item $p (Join-Path $d ("usb$($v.DriveLetter)-" + ($n -replace '\\', '_'))) -Force } }
  }
}

Prog 55 'Collecting event logs'
# 7. event logs (full evtx of the last 14 days plus a readable list of errors and warnings)
Step 'event logs' {
  $d = Join-Path $Work 'events'; New-Item -ItemType Directory -Force $d | Out-Null
  $ms = 14 * 24 * 3600 * 1000
  foreach ($ln in 'System', 'Application', 'Setup') {
    # v1.5: all events of the last 14 days as text (anonymizable); the raw .evtx is not kept
    Get-WinEvent -FilterHashtable @{ LogName = $ln; StartTime = (Get-Date).AddDays(-14) } -MaxEvents 6000 -ErrorAction SilentlyContinue |
      Select-Object TimeCreated, Id, LevelDisplayName, ProviderName, @{ n = 'Message'; e = { ($_.Message -replace '\s+', ' ') } } |
      Format-Table -AutoSize -Wrap | Out-String -Width 400 | Set-Content (Join-Path $d "$ln-all.txt") -Encoding UTF8
    Get-WinEvent -FilterHashtable @{ LogName = $ln; Level = 1, 2, 3; StartTime = (Get-Date).AddDays(-14) } -MaxEvents 3000 -ErrorAction SilentlyContinue |
      Select-Object TimeCreated, Id, LevelDisplayName, ProviderName, @{ n = 'Message'; e = { ($_.Message -replace '\s+', ' ') } } |
      Format-Table -AutoSize -Wrap | Out-String -Width 400 | Set-Content (Join-Path $d "$ln-errors.txt") -Encoding UTF8
  }
  Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1074, 6005, 6006, 6008, 41, 12, 13, 20, 27 } -MaxEvents 200 -ErrorAction SilentlyContinue |
    Select-Object TimeCreated, Id, ProviderName, @{ n = 'Message'; e = { ($_.Message -replace '\s+', ' ') } } | Format-Table -AutoSize -Wrap |
    Out-String -Width 400 | Set-Content (Join-Path $d 'boots-and-shutdowns.txt') -Encoding UTF8
}

Prog 68 'Collecting setup logs and crash dumps'
# 8. Windows setup and driver install logs, crash dumps
Step 'setup logs and dumps' {
  $d = Join-Path $Work 'windows'
  foreach ($p in 'C:\Windows\INF\setupapi.dev.log', 'C:\Windows\INF\setupapi.setup.log', 'C:\Windows\Panther\setupact.log', 'C:\Windows\Panther\setuperr.log',
                 'C:\Windows\Panther\UnattendGC\setupact.log', 'C:\Windows\debug\netsetup.log',
                 'C:\Windows\Setup\Scripts\psw-setupcomplete.log', 'C:\Windows\Setup\Scripts\psw-specialize.log') { Copy-IfThere $p $d }   # v1.3: SetupComplete/specialize logs ("Just a moment..." hangs)
  Get-ChildItem 'C:\Windows\Minidump' -Filter *.dmp -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 5 |
    ForEach-Object { Copy-IfThere $_.FullName (Join-Path $d 'minidump') }
  Get-ChildItem 'C:\Windows\LiveKernelReports' -Recurse -File -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime |
    Format-Table -AutoSize | Out-String -Width 300 | Set-Content (Join-Path $d 'livekernelreports-list.txt')
  if (Test-Path 'C:\Windows\MEMORY.DMP') { Get-Item 'C:\Windows\MEMORY.DMP' | Select-Object Length, LastWriteTime | Out-String | Set-Content (Join-Path $d 'memory-dmp-info.txt') }
}

Prog 76 'Reading scheduled tasks and processes'
# 9. scheduled tasks and processes of PSWloader
Step 'tasks' {
  Out-Txt 'tasks.txt' (Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -match 'PSW|PS5' } |
    ForEach-Object { $i = $_ | Get-ScheduledTaskInfo -ErrorAction SilentlyContinue; [pscustomobject]@{ Name = $_.TaskName; State = $_.State; LastRun = $i.LastRunTime; LastResult = ('0x{0:X}' -f $i.LastTaskResult); Action = ($_.Actions | ForEach-Object { "$($_.Execute) $($_.Arguments)" }) -join '; ' } } |
    Format-List)
}

Prog 82 'Creating the graphics report'
# 10. graphics report (dxdiag, takes up to a minute)
Step 'dxdiag' {
  $p = Start-Process -FilePath "$env:SystemRoot\System32\dxdiag.exe" -ArgumentList "/t `"$(Join-Path $Work 'dxdiag.txt')`"" -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { try { $p.Kill() } catch { }; L 'dxdiag stopped after 90 s' }
}

Prog 88 'Collecting developer details'
# 10b. v1.5 (user 26.11.2026): developer details - ps5pwr live log, driver parameters, boot history, summary.json
Step 'developer details' {
  $d = Join-Path $Work 'dev'; New-Item -ItemType Directory -Force $d | Out-Null
  $cli = @('C:\BC250\drivers\ps5pwr\v1.10\ps5pwr.ps1', 'C:\ProgramData\PSWloader\tools\ps5pwr\ps5pwr.ps1') | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($cli) {
    foreach ($a in @(@('-Hotplug', 'ps5pwr-hotplug-and-fan.txt'), @('-Status', 'ps5pwr-status.txt'))) {
      try { & powershell -NoProfile -ExecutionPolicy Bypass -File $cli $a[0] -Log (Join-Path $d 'ps5pwr-last.txt') *>&1 | Out-String -Width 300 | Set-Content (Join-Path $d $a[1]) -Encoding UTF8 } catch { }
    }
  }
  foreach ($svc in 'ps5pwr', 'ps5nvmsi', 'ps5m2flt', 'bc250kmd', 'ps5wifi', 'ps5btx', 'ps5gbe') {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Services\$svc"
    if (Test-Path $k) { Get-ChildItem $k -Recurse -ErrorAction SilentlyContinue | ForEach-Object { "[$($_.Name)]"; $_ | Get-ItemProperty | Select-Object * -ExcludeProperty PS* | Format-List | Out-String -Width 400 } |
      Set-Content (Join-Path $d "params-$svc.txt") -Encoding UTF8 }
  }
  # last 10 starts: start time, how the previous run ended, bug check code
  $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 12, 41, 6008, 1001, 1074 } -MaxEvents 400 -ErrorAction SilentlyContinue | Sort-Object TimeCreated)
  # Windows logs how the previous run ended (41 power lost/hang, 6008 unexpected shutdown, 1001 bug check) during the NEXT start,
  # after its start event 12 - so those events describe the run before the latest start
  $boots = @()
  foreach ($e in $ev) {
    if ($e.Id -eq 12 -and $e.ProviderName -match 'Kernel-General') { $boots += [pscustomobject]@{ Start = $e.TimeCreated; Before = 'normal'; BugCheck = '' }; continue }
    if (-not $boots.Count) { continue }
    $bt = $boots[-1]
    if ($e.Id -eq 1001) { $bt.Before = 'bug check'; if ($e.Message -match '0x[0-9a-fA-F]{8}') { $bt.BugCheck = $Matches[0] } }
    elseif ($e.Id -eq 41 -and $bt.Before -eq 'normal') { $bt.Before = 'unexpected (power lost or hang)' }
    elseif ($e.Id -eq 6008 -and $bt.Before -eq 'normal') { $bt.Before = 'unexpected shutdown' }
  }
  $last = @($boots | Select-Object -Last 10)
  $last | Format-Table Start, Before, BugCheck -AutoSize | Out-String -Width 200 | Set-Content (Join-Path $d 'boot-history.txt') -Encoding UTF8
  $bad = @($last | Where-Object { $_.Before -ne 'normal' })
  if ($bad.Count -ge 2) { Chk 1 'Start history' "$($bad.Count) of the last $($last.Count) starts followed a crash or an unexpected shutdown (details in the zip)." }
  elseif ($last.Count) { Chk 0 'Start history' "$($last.Count - $bad.Count) of the last $($last.Count) starts after a normal shutdown." }
  # machine-readable summary for triage of many reports
  $drv = @{}; foreach ($svc in 'bc250kmd', 'ps5pwr', 'ps5gbe', 'ps5wifi', 'ps5btx', 'ps5nvmsi', 'ps5m2flt') {
    $e2 = Get-CimInstance Win32_PnPEntity -Filter "Service='$svc'" -ErrorAction SilentlyContinue | Select-Object -First 1
    $drv[$svc] = if ($e2) { [ordered]@{ version = (Get-PnpDeviceProperty -InstanceId $e2.PNPDeviceID -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data; error = $e2.ConfigManagerErrorCode } } else { $null } }
  $os = Get-CimInstance Win32_OperatingSystem; $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
  $vj = $null; if (Test-Path 'C:\ProgramData\PSWloader\version.json') { $vj = Get-Content 'C:\ProgramData\PSWloader\version.json' -Raw | ConvertFrom-Json }
  [ordered]@{ schema = 1; selfcheck = $Ver; created = (Get-Date).ToString('s'); pswloader = $(if ($vj) { $vj.version }); firmware = [string](Get-CimInstance Win32_BIOS).SMBIOSBIOSVersion
    console = $script:Console; windows = "$($cv.DisplayVersion) $($cv.CurrentBuild).$($cv.UBR)"; language = (Get-UICulture).Name; lastBoot = $os.LastBootUpTime.ToString('s')
    drivers = $drv; bootHistory = @($last | ForEach-Object { [ordered]@{ start = $_.Start.ToString('s'); before = $_.Before; bugCheck = $_.BugCheck } })
    checks = @($Checks | ForEach-Object { [ordered]@{ state = @('ok', 'note', 'problem')[$_.State]; name = $_.Name; detail = $_.Detail } }) } |
    ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Work 'summary.json') -Encoding UTF8
}

Prog 91 'Removing personal details'
# 10c. v1.5: anonymize - user and computer names, Wi-Fi names, MAC addresses, serial numbers, device UUID and e-mail addresses are
# replaced in every text file; raw event logs (.evtx, binary, carry the computer name) are not kept - their readable copies are
Step 'anonymize' {
  $map = [ordered]@{}
  $add = { param($v, $t) if ($v -and "$v".Trim().Length -ge 3 -and -not $map.Contains("$v".Trim())) { $map["$v".Trim()] = $t } }
  & $add $env:COMPUTERNAME '<computer>'
  $i = 0; foreach ($u in Get-ChildItem C:\Users -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -notin 'Public', 'Default', 'Default User', 'All Users' }) { $i++; & $add $u.Name "<user$i>" }
  & $add $env:USERNAME '<user>'
  try { foreach ($lu in Get-LocalUser -ErrorAction Stop) { if ($lu.FullName) { & $add $lu.FullName '<user-name>' } } } catch { }
  $j = 0; foreach ($l in (netsh wlan show profiles 2>$null)) { if ($l -match ':\s*(.+)$' -and $l -match 'All User Profile|Profil f') { $j++; & $add $Matches[1].Trim() "<wifi$j>" } }
  $k = 0; foreach ($sn in @((Get-PhysicalDisk -ErrorAction SilentlyContinue).SerialNumber) + @((Get-Disk -ErrorAction SilentlyContinue).SerialNumber) + @((Get-CimInstance Win32_BIOS).SerialNumber)) { if ($sn -and "$sn".Trim().Length -ge 4) { $k++; & $add "$sn".Trim() "<serial$k>" } }
  & $add (Get-CimInstance Win32_ComputerSystemProduct).UUID '<device-uuid>'
  & $add (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue).ProductId '<product-id>'
  $keys = @($map.Keys | Sort-Object Length -Descending)
  Get-ChildItem $Work -Recurse -File -Include *.evtx -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
  foreach ($f in Get-ChildItem $Work -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -match '^\.(txt|log|json|xml|csv|ini|cfg|reg|inf)$' -or $_.Extension -eq '' }) {
    try {
      $t = [IO.File]::ReadAllText($f.FullName); $o = $t
      foreach ($kk in $keys) { $t = [regex]::Replace($t, [regex]::Escape($kk), $map[$kk], 'IgnoreCase') }
      $t = [regex]::Replace($t, '\b([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b', '<mac>')
      $t = [regex]::Replace($t, '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}', '<e-mail>')
      if ($t -ne $o) { [IO.File]::WriteAllText($f.FullName, $t) }
    } catch { }
  }
  "Replaced in the text files: user and computer names, Wi-Fi names, MAC addresses, serial numbers, device UUID, product id, e-mail addresses ($($keys.Count) values). Raw event logs removed (readable copies kept). Crash dumps kept unchanged." |
    Set-Content (Join-Path $Work 'ANONYMIZED.txt') -Encoding UTF8
}

Prog 94 'Creating the zip file'
# 11. zip
L 'creating zip'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $Zip) { Remove-Item $Zip -Force }
# entry by entry with '/' separators (CreateFromDirectory in Windows PowerShell 5.1 writes '\', which other tools misread)
$za = [IO.Compression.ZipFile]::Open($Zip, [IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($f in Get-ChildItem -LiteralPath $Work -Recurse -File) {
    $rel = $f.FullName.Substring($Base.Length + 1).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
  }
} finally { $za.Dispose() }
Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
# keep the newest 10 support zips
Get-ChildItem $Base -Filter 'PSW-selfcheck-*.zip' -File -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -Skip 10 | Remove-Item -Force -ErrorAction SilentlyContinue
$mb = [math]::Round((Get-Item $Zip).Length / 1MB, 1)
Write-Host ''
Write-Host "Done: $Zip ($mb MB)"
Write-Host 'Send this file with your support request.'
Prog 100 'Done'; if ($ProgressFile) { try { Add-Content -LiteralPath $ProgressFile -Value "ZIP`t$Zip" -Encoding UTF8 } catch { } }
if (-not $NoExplorer) { Start-Process explorer.exe "/select,`"$Zip`"" }
