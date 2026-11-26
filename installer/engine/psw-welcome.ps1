# psw-welcome.ps1 v1.8 (PSWloader v1.2; v1.8: before the windows a silent health check with troubleshooting in the background (task PSWloader Fix), a window only when something is left or a restart finishes a repair; v1.7: "Buy me a coffee" no longer closes the window; v1.6: frameless window, Dashboard cards; v1.5: What's new cards without icons (blue bar), banner colours; v1.4: "What's new" as cards with icons, the announced features, Release notes button; v1.3: also "What's new in v1.2" after an update, same look and size; v1.2: WPF;
# v1.1: QR code; v1.0: plain window) - one-time windows after a new installation or an update.
# Started by the logon task "PSWloader Welcome" (PSWSetup on a new installation: -Welcome; the updater: -Welcome -WhatsNew).
# It waits for the first sign-in after the restart that follows the installation/update (the boot must be newer than -After),
# then shows
#   - Welcome (Buy Me a Coffee button, QR code "Scan with Phone") once per PC - mark HKLM\SOFTWARE\PSWloader WelcomeShown=1
#     (or ProgramData\PSWloader\welcome-shown.txt); never again, also not after later updates;
#   - What's new in this version once per version - mark WhatsNewShown=<version>; after the welcome if both are due.
# When nothing is due any more the task removes itself. -Preview <png> [-Page Welcome|WhatsNew] renders a page into a picture.
param([long]$After = 0, [switch]$Welcome, [switch]$WhatsNew, [string]$Preview = '', [ValidateSet('Welcome', 'WhatsNew')][string]$Page = 'Welcome', [switch]$Show)   # -Show: the -Page once, no marks (Dashboard)
$ErrorActionPreference = 'Stop'
$Version = 'v1.2'
$Url = 'https://buymeacoffee.com/mrnative'
$NotesUrl = 'https://github.com/mr-Native/PSWloader/releases'
$key = 'HKLM:\SOFTWARE\PSWloader'
$mark = "$env:ProgramData\PSWloader\welcome-shown.txt"
$task = 'PSWloader Welcome'
function Remove-WelcomeTask { Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue }
function Get-Mark([string]$Name) { (Get-ItemProperty -Path $key -Name $Name -ErrorAction SilentlyContinue).$Name }
function Set-Mark([string]$Name, $Value) {
  try { New-Item -Path $key -Force -ErrorAction SilentlyContinue | Out-Null; Set-ItemProperty -Path $key -Name $Name -Value $Value -ErrorAction SilentlyContinue } catch { }
}

$showWelcome = $false; $showNews = $false
if ($Preview -or $Show) { $showWelcome = $Page -eq 'Welcome'; $showNews = $Page -eq 'WhatsNew' }
else {
  $welcomeDone = (Get-Mark 'WelcomeShown') -eq 1 -or (Test-Path -LiteralPath $mark)
  $showWelcome = $Welcome -and -not $welcomeDone
  $showNews = $WhatsNew -and (Get-Mark 'WhatsNewShown') -ne $Version
  if (-not $showWelcome -and -not $showNews) { Remove-WelcomeTask; exit 0 }
  $boot = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).LastBootUpTime
  if ($After -gt 0 -and $boot -and $boot.ToFileTimeUtc() -le $After) { exit 0 }   # same boot as the installation/update: wait
}

Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
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

# QR code of $Url (qrencode -s 10 -m 0 -l M, 29 modules, 290 x 290 px), drawn pixel-exact
$qrPng = 'iVBORw0KGgoAAAANSUhEUgAAASIAAAEiAQMAAABncE31AAAABlBMVEUAAAD///+l2Z/dAAAAAnRSTlP//8i138cAAAAJcEhZcwAACxIAAAsSAdLdfvwAAAGVSURBVGiB7ZnLDYQwDEQtbQEpidYpiQKQvGT8C3uO9zQ+RAE/TiN/EXH7qJ7jFjmueXteqF7hO4RUC3XoNPiGmgE13h5JNVFTCNPlUQi6xO1Bp3Sk/kAhFAYUElJ/pVSnLtPOmahI9VORmZzX+/2lkmqj3D5LLYgjfKR6KP0xtETmq5ekWiiEA1IRXBUnXpZJdVKovrgOHLeLc4p3p1E7SG2nALzMxMk4MR1JNVBWh6cQ0KU60ajNpJooe4AhRynqMOKkthGkWqgApuUYhnZUYhGR0UNqL2W7HqsAdViIeJWeOpJqoNysDschi0y3VNUgtZWqGKmk5MXYOlFSfdQp1YlCHJ9+zeGdKKkOKtY8tWnOPVstgUi1ULHWV13FyTFMSfVRPgBAlxGjQGYr+0hJtVCatvBemzVChFQLFeZNqPlkrHz1TKT2UgfkedWCRTB8TqqLqj9ZGRi+eZAIEVLNlN18CshKIetejlQTZX2RqoYvouNXR1LbqMhM3n9e4pZTwCt/kdpIuXlV8H+MORS8d0OkdlJfix6mko20RWEAAAAASUVORK5CYII='

# common look (800 wide, title, content, grey footer with buttons on the left) - psw-firstlogin-status.ps1 uses the same
$style = @'
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
              <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="B" Property="BorderBrush" Value="#F2F2F0"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Head" TargetType="TextBlock"><Setter Property="FontFamily" Value="Segoe UI Semibold"/><Setter Property="FontSize" Value="24"/>
      <Setter Property="Foreground" Value="#F2F2F0"/><Setter Property="Margin" Value="0,0,0,16"/></Style>
    <Style x:Key="Body" TargetType="TextBlock"><Setter Property="FontFamily" Value="Segoe UI"/><Setter Property="FontSize" Value="14"/>
      <Setter Property="LineHeight" Value="21"/><Setter Property="Foreground" Value="#DADAD5"/><Setter Property="TextWrapping" Value="Wrap"/></Style>
  </Grid.Resources>
  <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
'@
$open = '<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Background="#1B1B1B" Width="800" TextOptions.TextFormattingMode="Display" UseLayoutRounding="True" SnapsToDevicePixels="True">'
$footer = @'
  <Border Grid.Row="1" Background="#151515" BorderBrush="#2C2C2C" BorderThickness="0,1,0,0" Padding="36,18,36,20">
    <StackPanel Orientation="Horizontal">BUTTONS</StackPanel>
  </Border>
</Grid>
'@

$welcomeXaml = $open + $style + @'
  <Grid Grid.Row="0" Margin="36,30,36,28">
    <Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
    <StackPanel Grid.Column="0" Margin="0,0,32,0">
      <StackPanel Orientation="Horizontal" Margin="0,0,0,18"><Grid Width="30" Height="30"><Ellipse Fill="#3A86E0"/><Path Data="M10,9 L15.5,15 L10,21 M16,9 L21.5,15 L16,21" Stroke="#1B1B1B" StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round"/></Grid><TextBlock Margin="10,0,0,0" VerticalAlignment="Center" FontFamily="Segoe UI" FontWeight="Bold" FontSize="20"><Run Text="PSW" Foreground="#F2F2F0"/><Run Text="loader" Foreground="#3A86E0"/></TextBlock></StackPanel>
      <TextBlock Style="{StaticResource Head}" Text="Welcome to PSWloader!"/>
      <TextBlock Style="{StaticResource Body}">
        <Run Text="This project started out of personal interest and has no commercial purpose. However, if you enjoy using PSWloader, I would really appreciate your support on Buy Me a Coffee. Your contribution helps me continue expanding, improving and updating the project in the long run."/>
        <LineBreak/><LineBreak/>
        <Run Text="You can support me with the button below, or scan the code with your phone."/>
        <LineBreak/><LineBreak/>
        <Run Text="Regardless of whether you choose to support the project, this message will only be shown to you once."/>
        <Run x:Name="Smile" FontFamily="Segoe UI Emoji"/>
      </TextBlock>
    </StackPanel>
    <Border Grid.Column="1" VerticalAlignment="Top" Background="#242424" BorderBrush="#333333" BorderThickness="1" CornerRadius="12" Padding="16,16,16,10">
      <StackPanel>
        <Border Background="White" CornerRadius="6" Padding="16">
          <Image x:Name="Qr" Width="174" Height="174" RenderOptions.BitmapScalingMode="NearestNeighbor"/>
        </Border>
        <TextBlock Text="Scan with Phone" FontFamily="Segoe UI Semibold" FontSize="13" Foreground="#B4B4AE" HorizontalAlignment="Center" Margin="0,10,0,0"/>
      </StackPanel>
    </Border>
  </Grid>
'@ + $footer.Replace('BUTTONS', @'
<Button x:Name="Coffee" Style="{StaticResource Btn}" Content="Buy me a coffee" Background="#3A86E0" Foreground="#FFFFFF" BorderBrush="#3A86E0" BorderThickness="1" IsDefault="True"/>
      <Button x:Name="Close" Style="{StaticResource Btn}" Content="Close" Background="#262626" Foreground="#F2F2F0" BorderBrush="#454545" BorderThickness="1" Margin="12,0,0,0" IsCancel="True" MinWidth="110"/>
'@)

# What's new: the announcement (X/Twitter) plus a few more, as cards - icon (Segoe MDL2 Assets), title, one short line
$news = @(
  @([char]0xE9D9, 'PSWloader Dashboard', 'Overview, health check and tools in one place.'),
  @([char]0xE90F, 'Self check and troubleshooting', 'Finds problems and can fix them (experimental).'),
  @([char]0xE702, 'Native Bluetooth', 'Connect Bluetooth devices.'),
  @([char]0xE701, 'More stable Wi-Fi, disks and GPU', 'Fixes in the Wi-Fi, NVMe and GPU drivers.'),
  @([char]0xE7F4, 'HDMI hotplug and resolutions', 'Picture back after replug or display off.'),
  @([char]0xEDA2, 'USB and NVMe boot fixes', 'Reliable starts from USB and M.2 SSDs.'),
  @([char]0xE7FC, 'Initial PS5 Pro support', 'Windows starts, no GPU driver yet.'),
  @([char]0xE74F, 'Quieter fan', 'No loud spin-up at start any more.'),
  @([char]0xE950, 'Video memory fixes', 'More video memory for the GPU by default.'),
  @([char]0xE72C, 'Boot once', 'Restart straight into another disk.')
)
$cards = ($news | ForEach-Object {   # v1.5: no icons - a thin PSWloader-blue bar on the left of each card
  @"
      <Border Background="#242424" BorderBrush="#333333" BorderThickness="1" CornerRadius="8" Margin="0,0,10,10" ClipToBounds="True">
        <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="4"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
          <Border Background="#3A86E0" CornerRadius="8,0,0,8"/>
          <StackPanel Grid.Column="1" Margin="14,11,12,11" VerticalAlignment="Center">
            <TextBlock Text="$([Security.SecurityElement]::Escape($_[1]))" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#F2F2F0" TextWrapping="Wrap"/>
            <TextBlock Text="$([Security.SecurityElement]::Escape($_[2]))" FontFamily="Segoe UI" FontSize="12.5" Foreground="#B4B4AE" TextWrapping="Wrap" Margin="0,2,0,0"/>
          </StackPanel>
        </Grid>
      </Border>
"@ }) -join "`n"
$newsXaml = $open + $style + @"
  <StackPanel Grid.Row="0" Margin="36,28,24,16">
    <StackPanel Orientation="Horizontal" Margin="0,0,0,18"><Grid Width="30" Height="30"><Ellipse Fill="#3A86E0"/><Path Data="M10,9 L15.5,15 L10,21 M16,9 L21.5,15 L16,21" Stroke="#1B1B1B" StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round"/></Grid><TextBlock Margin="10,0,0,0" VerticalAlignment="Center" FontFamily="Segoe UI" FontWeight="Bold" FontSize="20"><Run Text="PSW" Foreground="#F2F2F0"/><Run Text="loader" Foreground="#3A86E0"/></TextBlock></StackPanel>
    <StackPanel Orientation="Horizontal" Margin="0,0,0,4">
      
      <TextBlock Style="{StaticResource Head}" Text="What's new in PSWloader" Margin="0"/>
      <Border Background="#3A86E0" CornerRadius="12" Padding="10,2" Margin="12,4,0,0" VerticalAlignment="Center">
        <TextBlock Text="$Version" FontFamily="Segoe UI Semibold" FontSize="14" Foreground="#FFFFFF"/>
      </Border>
    </StackPanel>
    <TextBlock Style="{StaticResource Body}" Foreground="#B4B4AE" Text="This is what changed." Margin="0,0,12,14"/>
    <UniformGrid Columns="2">
$cards
    </UniformGrid>
  </StackPanel>
"@ + $footer.Replace('BUTTONS', @'
<Button x:Name="Close" Style="{StaticResource Btn}" Content="Close" Background="#3A86E0" Foreground="#FFFFFF" BorderBrush="#3A86E0" BorderThickness="1" IsDefault="True" IsCancel="True" MinWidth="110"/>
      <Button x:Name="Notes" Style="{StaticResource Btn}" Content="Release notes" Background="#262626" Foreground="#F2F2F0" BorderBrush="#454545" BorderThickness="1" Margin="12,0,0,0"/>
'@)

function New-Root([string]$Xaml) { [Windows.Markup.XamlReader]::Parse($Xaml) }
function Show-Root($Root) {
  $w = New-Object System.Windows.Window
  $w.Title = 'PSWloader'; $w.Content = $Root; $w.SizeToContent = 'WidthAndHeight'; $w.ResizeMode = 'NoResize'
  $w.WindowStartupLocation = 'CenterScreen'; $w.Topmost = $true; $w.ShowInTaskbar = $true
  $Root.FindName('Close').Add_Click({ [System.Windows.Window]::GetWindow($this).Close() })
  $c = $Root.FindName('Coffee'); if ($c) { $c.Add_Click({ Start-Process $Url }) }   # v1.7 (user 26.11.2026): the window stays - only Close ends it
  $n = $Root.FindName('Notes'); if ($n) { $n.Add_Click({ Start-Process $NotesUrl }) }
  Set-PswDarkTitle $w; [void]$w.ShowDialog()
}
function Get-WelcomeRoot {
  $r = New-Root $welcomeXaml
  $qr = New-Object System.Windows.Media.Imaging.BitmapImage
  $qr.BeginInit(); $qr.StreamSource = New-Object IO.MemoryStream (, [Convert]::FromBase64String($qrPng)); $qr.CacheOption = 'OnLoad'; $qr.EndInit(); $qr.Freeze()
  $r.FindName('Qr').Source = $qr
  $r.FindName('Smile').Text = ' ' + [char]::ConvertFromUtf32(0x1F60A)
  $r
}

if ($Preview) {   # layout check only: render the page into a PNG (2x for a sharp check)
  $root = if ($Page -eq 'Welcome') { Get-WelcomeRoot } else { New-Root $newsXaml }
  $root.Measure([System.Windows.Size]::new([double]::PositiveInfinity, [double]::PositiveInfinity))
  $root.Arrange([System.Windows.Rect]::new($root.DesiredSize)); $root.UpdateLayout()
  $s = 2.0; $bmp = [System.Windows.Media.Imaging.RenderTargetBitmap]::new([int]($root.ActualWidth * $s), [int]($root.ActualHeight * $s), 96 * $s, 96 * $s, [System.Windows.Media.PixelFormats]::Pbgra32)
  $bmp.Render($root); $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder; $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bmp))
  $fs = [IO.File]::Create($Preview); $enc.Save($fs); $fs.Close(); "preview $Page $Preview $([int]$root.ActualWidth)x$([int]$root.ActualHeight)"; exit 0
}

# >>> psw-ui v1.1 (PSWloader v1.2) - the look of the PSWloader windows (WPF, 800 wide, title, text, grey footer, buttons on
# the left, first button blue (PSWloader banner colours)). Source: installer/engine/psw-ui.ps1, copied between these markers into the scripts that show
# windows (tools/psw-display.ps1, tools/psw-uefi-bootmanager.ps1, installer/updater/PSWUpdateCheck.ps1) by tools/sync-psw-ui.py.
#   Show-PswDialog -Bar -Head -Text -One [-Two] [-Three] [-Stay @{ 1 = { ... } }]  -> 1, 2 or 3 (closing the window = the last button);
#     a button listed in -Stay only runs its script block (open a folder, a web page) and the window stays (user 26.11.2026:
#     a window must not disappear because something was opened from it - only its closing button ends it)
#   Show-PswChoice -Bar -Head -Text -Items -Selected -One -Two -> index of the chosen item, -1 = second button / closed
#   -Preview <png> on both: render instead of showing
# v1.3 (HW 26.11.2026: after a cold start the Dashboard window on the Realtek Windows stayed a transparent frame - WPF's
# hardware rendering goes through D3D9 of the PS5 GPU driver and presented nothing; software rendering drew it at once):
# every PSWloader window renders in software - independent of the GPU driver (broken driver, PS5 Pro without driver)
try { Add-Type -AssemblyName PresentationCore; [System.Windows.Media.RenderOptions]::ProcessRenderMode = [System.Windows.Interop.RenderMode]::SoftwareOnly } catch { }
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
function New-PswUiRoot([string]$Head, [string]$Text, [string[]]$Buttons, [string]$Middle = '') {
  Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
  $e = { param($t) [Security.SecurityElement]::Escape($t) -replace "`n", '&#x0a;' }
  $b = ''; for ($i = 0; $i -lt $Buttons.Count; $i++) {
    $style = if ($i -eq 0) { 'Background="#3A86E0" Foreground="#FFFFFF" BorderBrush="#3A86E0" IsDefault="True"' } else { 'Background="#262626" Foreground="#F2F2F0" BorderBrush="#454545" Margin="12,0,0,0" MinWidth="110"' }
    if ($i -eq $Buttons.Count - 1 -and $Buttons.Count -gt 1) { $style += ' IsCancel="True"' }
    $b += "<Button x:Name=""B$($i + 1)"" Style=""{StaticResource Btn}"" Content=""$(& $e $Buttons[$i])"" BorderThickness=""1"" $style/>"
  }
  [Windows.Markup.XamlReader]::Parse(@"
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
              <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="B" Property="BorderBrush" Value="#F2F2F0"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="RadioButton">
      <Setter Property="FontFamily" Value="Segoe UI"/><Setter Property="FontSize" Value="14"/><Setter Property="Margin" Value="0,5,0,5"/>
      <Setter Property="Foreground" Value="#F2F2F0"/><Setter Property="Cursor" Value="Hand"/><Setter Property="VerticalContentAlignment" Value="Center"/><Setter Property="Padding" Value="10,0,0,0"/>
    </Style>
  </Grid.Resources>
  <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
  <StackPanel Grid.Row="0" Margin="36,30,36,28">
    <StackPanel Orientation="Horizontal" Margin="0,0,0,18"><Grid Width="30" Height="30"><Ellipse Fill="#3A86E0"/><Path Data="M10,9 L15.5,15 L10,21 M16,9 L21.5,15 L16,21" Stroke="#1B1B1B" StrokeThickness="3.2" StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round"/></Grid><TextBlock Margin="10,0,0,0" VerticalAlignment="Center" FontFamily="Segoe UI" FontWeight="Bold" FontSize="20"><Run Text="PSW" Foreground="#F2F2F0"/><Run Text="loader" Foreground="#3A86E0"/></TextBlock></StackPanel>
    <TextBlock Text="$(& $e $Head)" FontFamily="Segoe UI Semibold" FontSize="24" Foreground="#F2F2F0" Margin="0,0,0,16" TextWrapping="Wrap"/>
    <TextBlock Text="$(& $e $Text)" FontFamily="Segoe UI" FontSize="14" LineHeight="21" Foreground="#DADAD5" TextWrapping="Wrap"/>
    $Middle
  </StackPanel>
  <Border Grid.Row="1" Background="#151515" BorderBrush="#2C2C2C" BorderThickness="0,1,0,0" Padding="36,18,36,20">
    <StackPanel Orientation="Horizontal">$b</StackPanel>
  </Border>
</Grid>
"@)
}
function Show-PswUiRoot($Root, [string]$Bar, [string]$Preview) {
  if ($Preview) {
    $Root.Measure([System.Windows.Size]::new([double]::PositiveInfinity, [double]::PositiveInfinity)); $Root.Arrange([System.Windows.Rect]::new($Root.DesiredSize)); $Root.UpdateLayout()
    $bmp = [System.Windows.Media.Imaging.RenderTargetBitmap]::new([int]($Root.ActualWidth * 2), [int]($Root.ActualHeight * 2), 192, 192, [System.Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($Root); $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder; $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $fs = [IO.File]::Create($Preview); $enc.Save($fs); $fs.Close(); return
  }
  $w = New-Object System.Windows.Window
  $w.Title = $Bar; $w.Content = $Root; $w.SizeToContent = 'WidthAndHeight'; $w.ResizeMode = 'NoResize'
  $w.WindowStartupLocation = 'CenterScreen'; $w.Topmost = $true; $w.ShowInTaskbar = $true
  for ($i = 1; $i -le 3; $i++) { $btn = $Root.FindName("B$i"); if ($btn) { $btn.Tag = $i; $btn.Add_Click({
    $n = [int]$this.Tag
    if ($script:PswUiStay -and $script:PswUiStay.ContainsKey($n)) { try { & $script:PswUiStay[$n] } catch { } }   # opens something, the window stays
    else { $script:PswUiResult = $n; [System.Windows.Window]::GetWindow($this).Close() } }) } }
  Set-PswDarkTitle $w; [void]$w.ShowDialog()
}
function Show-PswDialog([string]$Bar, [string]$Head, [string]$Text, [string]$One, [string]$Two = '', [string]$Three = '', [string]$Preview = '', [hashtable]$Stay = @{}) {
  $btns = @($One) + @(@($Two, $Three) | Where-Object { $_ })
  $script:PswUiResult = $btns.Count; $script:PswUiStay = $Stay
  $r = New-PswUiRoot $Head $Text $btns
  Show-PswUiRoot $r $Bar $Preview
  $script:PswUiStay = @{}
  $script:PswUiResult
}
function Show-PswChoice([string]$Bar, [string]$Head, [string]$Text, [string[]]$Items, [int]$Selected, [string]$One, [string]$Two, [string]$Preview = '') {
  $e = { param($t) [Security.SecurityElement]::Escape($t) }
  $rows = ''; for ($i = 0; $i -lt $Items.Count; $i++) { $rows += "<RadioButton x:Name=""R$i"" GroupName=""C"" Content=""$(& $e $Items[$i])""$(if ($i -eq $Selected) { ' IsChecked="True"' })/>" }
  $mid = "<Border Background=""#242424"" BorderBrush=""#333333"" BorderThickness=""1"" CornerRadius=""10"" Padding=""18,10"" Margin=""0,18,0,0""><StackPanel>$rows</StackPanel></Border>"
  $script:PswUiResult = 2; $script:PswUiStay = @{}
  $r = New-PswUiRoot $Head $Text @($One, $Two) $mid
  Show-PswUiRoot $r $Bar $Preview
  if ($script:PswUiResult -ne 1) { return -1 }
  for ($i = 0; $i -lt $Items.Count; $i++) { if ($r.FindName("R$i").IsChecked) { return $i } }
  -1
}
# <<< psw-ui

# v1.8 (user 26.11.2026: the first sign-in message said everything was installed while the GPU driver failed at the next start):
# before the welcome / What's new window, a silent health check with troubleshooting (SYSTEM task "PSWloader Fix": check,
# fix what it can - drivers again, GPU state, devices, tasks -, check again). Only when something is left, or a repair needs
# a restart, a window says so.
function Invoke-SilentRepair {
  $dd = "$env:ProgramData\PSWloader\dashboard"; $prog = "$dd\fix-progress.txt"
  if (-not (Get-ScheduledTask -TaskName 'PSWloader Fix' -ErrorAction SilentlyContinue)) { return }
  $t0 = Get-Date
  try { Start-ScheduledTask -TaskName 'PSWloader Fix' -ErrorAction Stop } catch { return }
  for ($k = 0; $k -lt 120; $k++) {   # at most 4 minutes
    Start-Sleep -Seconds 2
    $f = Get-Item -LiteralPath $prog -ErrorAction SilentlyContinue
    if ($f -and $f.LastWriteTime -gt $t0 -and @(Get-Content -LiteralPath $prog -ErrorAction SilentlyContinue) -contains 'DONE') { break }
  }
  $res = $null; try { $res = Get-Content "$dd\fix-result.json" -Raw | ConvertFrom-Json } catch { }
  $ch = $null; try { $ch = Get-Content "$dd\checks.json" -Raw | ConvertFrom-Json } catch { }
  $left = @(@($ch.checks) | Where-Object { $_.state -eq 2 })
  $fixed = @(@($res.actions) | Where-Object { $_.ok -and $_.name -ne 'Check' })
  $dash = "$env:ProgramFiles\PSWloader\PSWDashboard.ps1"
  if ($left.Count) {
    $list = ($left | Select-Object -First 4 | ForEach-Object { "-  $($_.name): $($_.detail)" }) -join "`n"
    $r = Show-PswDialog -Bar 'PSWloader' -Head 'PSWloader found a problem' -One 'Open PSWloader Dashboard' -Two 'Close' `
           -Text "PSWloader checked this Windows after the installation and could not fix this by itself:`n`n$list`n`nThe Dashboard shows the details and creates a support zip for an issue on GitHub."
    if ($r -eq 1 -and (Test-Path $dash)) { Start-Process "$env:SystemRoot\System32\conhost.exe" -ArgumentList "--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$dash`" -Page Health" }
  } elseif ($fixed.Count -and $res.restart) {
    $list = ($fixed | ForEach-Object { "-  $($_.name)" }) -join "`n"
    if ((Show-PswDialog -Bar 'PSWloader' -Head 'PSWloader repaired a driver' -One 'Restart now' -Two 'Later' `
           -Text "PSWloader checked this Windows after the installation and repaired:`n`n$list`n`nRestart Windows to finish.") -eq 1) {
      Start-Process "$env:SystemRoot\System32\shutdown.exe" -ArgumentList '/r /t 5 /c "PSWloader"' -WindowStyle Hidden; exit 0 }
  }
}

# each page: mark first (also if it is closed some other way), then show; the task goes when nothing is left
if ($Show) { if ($showWelcome) { Show-Root (Get-WelcomeRoot) } else { Show-Root (New-Root $newsXaml) }; exit 0 }   # Dashboard: no marks, no task changes
Invoke-SilentRepair   # v1.8
if ($showWelcome) {
  Set-Mark 'WelcomeShown' 1
  try { New-Item -ItemType Directory -Force -Path (Split-Path -Parent $mark) -ErrorAction SilentlyContinue | Out-Null; Set-Content -LiteralPath $mark -Value (Get-Date -Format s) -ErrorAction SilentlyContinue } catch { }
  if (-not $showNews) { Remove-WelcomeTask }
  Show-Root (Get-WelcomeRoot)
}
if ($showNews) {
  Set-Mark 'WhatsNewShown' $Version
  Remove-WelcomeTask
  Show-Root (New-Root $newsXaml)
}
