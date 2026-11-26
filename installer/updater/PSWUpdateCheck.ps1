# PSWUpdateCheck.ps1 (v1.2: windows in the PSWloader look) - Silent boot-time update checker for PSWloader on Windows 11.
# Reads C:\ProgramData\PSWloader\updater.json and C:\ProgramData\PSWloader\version.json.
# Exits silently if disabled, offline, or already up to date.
# If a newer GitHub release is available, prompts the user to download & run the Updater,
# skip for now, or permanently disable update checks.
param(
    [switch]$Force,
    [string]$OverrideLatest = '',
    [string]$Preview = '',  # v1.2: render the update window into a picture (layout check)
    [switch]$Beta           # v1.2: also offer pre-releases (beta channel); the PSWloader Dashboard turns it on per user (HKCU\Software\PSWloader BetaUpdates=1)
)

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
          $png = 'iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAYAAABXAvmHAAAMDklEQVR42t1af3BV5Zl+3vf7zr333B8JIRCw0NgEKoqLU5GlSMvGCkxr1e6Ukqwzu+7MTp11rJWxnaWuxhqoYmvtrFM73V233dmhnZ2xCWPX6Uor/QFUViIrLquCC0JQFDSQhCQ3995zz/m+790/zk1ASkJucDtbz8z95853znme93t/PO/7HUK1V0cHt165gbrayI7+tfyRtxeQ0CdF3LWA/JEIPkxwdQD8ypKSgE8T4S2AXiXi3UKy6/l75h4cfUZrp6iu/RsEGze6auBQNWtbO4VHgS976Ngc5am1gKwR55YoL5UGMcQZiDUQsYBI5U4CkQIpDWINiIONgiIxvwjQUzayW7rvbzw+RqSNHAB5/wh0CGMjOQBY8eAbTS6R+Arg/lIlc7ViQ7iwBCfWQSBEoPi5dM6zRQCICAQEYlLMCR+kErDl/BDAP+IwfOy5r3/k6LnvvCgCFYvYa/76Cc9vvvFrwrxeeelaGwzDiTUAEUH4dwFf6BIRkANEmJRWqRrYqDhEzj1a6nnm23v/6fZo9N1TJtDSsV3v3Pgps+wbRxapZOqHKpldaktDcGINAap60BORgWVSWvm1sOWRPbYc3Nb9wLxXRjFUTaClQ/TOjWSWbeppU57/z6x01gR5QyAFer+A/y4PgVidymlnzYiNSl/sbm/uHMUyaQKjNyzf1HOXSuUed1EAZyNLxAq/h0vEWVaeYi8FG+TXPd/e/L3xSNBE4HW67nETDFuII4AYv9dLHIhFp2qUKZ4elwSdL2Cv3XRkrU7VdtlywUIc/5+5zKTinJ1KZpQJhlp3t8/bcm5g85n6FOf4pZsOLWTP3+xM4ABHE4IXF/8mRlFZJ9UTICLAkTOBY8/fvHTToYVdbWQ7OoTPISB04MouuuaJFz1NyR+z8tLOhDK+2wggAvLSIC89muLPT5AUOJEFKW8SZM/Lgp0JhZWX1pT88TVPvOgduLKLAKExAq2d4K62Npvsq1/vZeoWmyBvxg9YAUiBvBTC3gMIew+AvBRA6r1WFgdKpCFRCaW39sAU+sCJ7JRIELEyQd54mbrFyb769V1tbba1M8ZO6BDGBsiKhw7PtTr1Ggg+nKHxczwBcDj1bAcKB38BAMgs+DQaPvMwBA5wsQogL43yiX04ufUemOET4GQO9dffi9yiL8AVBwDWVQcEWAsEJWWCK567f/7b2ADiFuxgEElE+h7t5zLijBsXvLPgZBb5V55Cft+TIC8J0knkX96CU9seAHv+GZIuQv/2byLqPwJOZODKIzi19V4UDv0SnJ4OOFP1PogzTvu5TET6HhBJC3YwAcDHNx2ZpVgfJlBGxGJCAunpOLX1bzH83z+B8msh1gCs4Yr9qF16G2as+jpcOAIXDOH4j9bChYVRVwZcBJDC7LU/gN+4FK40WOVOiBApCKRgnZn/Qvu8Xo4DQd+i/WlZJ9ZOKA8IgDPwGz8OsWEsABD/x+npGNrzQ5ze9Tg4kYPy65GYtTAGqTxALMAexEbo/be7UO49AE7VVLkTRE6s1f60LEPfciYLkWsVG8oFtQ0puPIIMld8FnWfuAuueBoYjXVnwX4dBp57DEMv/gvYr8WMlfcjcckiuGAotrRYkJeCC4bQ+9SXEA2+BUpkAWercyUbCsi1AoBa9vCx+QQ8CGc9QHgS9wMuQuajK+GCQQRv7o5BVLIL6SSKh38DnZuNzGWr4c9dgmLPTrhiP0inAGdAOglb7EPpzW5kF3wGnMzEOzGJekkAw1kSoGHuyq/+RDWu+vIanciusbbsaLLqUgCxZWTmr4IZehvl43vPkKg0L4XDv4ZX14R00wokL7kKxdd/CVceAakkIAbkpWGG30ZwYh+yl98Y1wlnJ0VC4JxOZDy40itqzvVfvVN5iaudDR1NVu8QVWqZReay1SifPIjw5H5QIlPxdQWIQ/Hwr5GcdSXSzSuQmLkAhYO/gNio0pUZkJdB1H8YYd8hZK+46Uzfc2ESjnWSbWT6VOOqu9uJ1Zw4mKrQPERxsSJG5qOrERzfi2igB+RlYnDsQUyA0pEdSM1djHTTn8CruxSFgz+PBRhzpdhlEPbuhxk6gezlN0KcqVT1iXMJsSI4E6m519/9IBGycA5VizYiwFmQTiIzfyWKR3fB5E+APD8moZNw5WGU3vgPpBqXIt20AjrbEJNgHUMRB0pkUT7+ElwwjMxlqyAmmtiWEuskcU6pD6+8+yEAusoG/70kbARO1SDd3ILi67+CKw6AdDLOOtqHHTmJ8vGXkG5uQbpxGTiVQ+Hgs/GaMdmRRemNXSCVRLrpkxBTGl+KjRpaRDMRUlMTWWddrCBhAbrmQ5i99gfQNR+C2LBSvAw4VYvyOy+jdPQ5iCmjdvGtqF95H8QEZ0CKBSdzGH65C7bYD7A38WBCHIiQep+bFImtOmHjFvv3mPUvYs4DACyC4KKbLWdBng9b6MM7XbchGjwGUok4rbKCC4aRnL0I6aYVIJ3C8Mud6Nu2Ma4Lo7tPCq6cR+6qtVDp+lh2TESHGCIIGMAgMeOMLphC56eTcFER7/70TkQDR8HJXCWdakhYhK6ZjYabHoWuuxT5Az9D37MPxMWr4so0qqWu/nNMu/YOuDAfy/MJOjViBoBBJsIxYg0QZCrgwRoigt6n16F8Yh84VVupqgpiyuBkFrM+912k5i5B4dA2nNr6tbhokRq73xYHkLn8s5jx6W9AomAy4zgh1iDCMXaCV4kVBFUSkLhhI5XAqWfWo9TzW7BfN6Y44QyIGA03/x3SzStQPLoLJ3/2lTjPV3QR2IMrnYbf9Ak03PQdiI3i/y8QCQIIsYITvMrMdnelk6JqghUQcCKLvm0dGHnt38Hp+orl4wIlNsLMG76JzIIbEBzfh96n18EF+YrfV8AHQ0hechVm/enjoErGmmQ8EkTAbHezE2+HCYsBk+Jq8HN6Ovp3fBvD//WvZzUosQ1cWMCM1R2o+difIeo7hN6n74LNvwtKpON1rCHlEXjTP4LZn/97cDIHMeVJT26YFJuwGDjxdnD3fY2HAdnLCV8EsJNxHfJ8DOx4FIPd/1hxGzuWGVxpCNNb/ga1f/xXKPcewLs/vRNh3xFQMlexsIZEJajMDMxe8w9QuVmQsHiB1Pse21lO+ALI3u77Gg/HlIW7SCXogrMPiVvKwv9sxcBvHwOnpp1Jg6zgSgOYtvwO1F17B1wwhL5fPYjg+EtQfu1YbIgtgz0fsz7/fSTq50PK+UmDH+vKVIIg3DXW0DiYJ01pcIRJqQlJCADWKB17AaQTID47DQ6gZvFfoL5lPVxYgC32I+w9AOXXxcFJqrJThIbPfRepOVfDjjY6VYBnUsqUBkcczJMAwC0d2/UL7fN6nXWblV9DF3QjcUjMvAwSFiHiAGKYkZPILrwZM1ZviHtgZ6D8OuiaS+CCIRBriA3hTAkNNz6C9Lzr4EqnK4KuqtRhlV9DzrrNL7TP623p2K55J65zECFPzCOmlC8Qax53FzhuKXOL1iD3sVsgURliy8gt+gJm3vCtWP+MuZSH+k/dC69+HlxUBCcyaLjhW8guvBmu2D+lsQqxZlPKFzwxj0CEduI6R2fPRJdvOnqfl63fFI70GRrXPJXBlvIQ9r4GAEjMuiJ2k7M7qspgy5WGEPa9Dl07F17tHLhyfkpzYhFnEtkZOhrpb3++venhUcyjb6PWzi7uOd3Myf6Z3SqZXmyCvJ1wOieIdT8AiUqVDErnrdSsU/EUY1ShTmHcrlM5ZcvFl8r1p5Y11/W4rrZWB9BoE0+ycH+r7L19SWSkfKuzUZF1gsafA8aDO4mKkKhYsTqdV3BBLFw4UglknpLYYp0gZ6OikfKte29fEi3c3yoAyQdrvA4AXW1kWzpE726ft8UGw+u0X6PA7HDRHc8UZS6z036NssHwut3t87a0dIg+99Dvg3fE9Id2yDduVO3cSKalY7vubm/utEFxuTPhHi9Tr4kVCcRM7chlImcXQ6zIy9RrZ8I9Niguj8FvHxc8AEzoEm/u3OxaO0Vt+9L0dxuaF2zWuUvLUOoanaxJi41IYnUmBKGLOOh2TEppfxqL2CFE5YeCnme++J/fWf1Oa6eorV9umvpB9wfiU4Oz1/7hfuzxnt34//W5zf8CYLHGjH9Kfw4AAAAASUVORK5CYII='
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

$ErrorActionPreference = 'SilentlyContinue'
$DataDir    = 'C:\ProgramData\PSWloader'
$ConfigPath = Join-Path $DataDir 'updater.json'
$VerPath    = Join-Path $DataDir 'version.json'
$LogDir     = 'C:\PSWSetup\Update'
$LogPath    = Join-Path $LogDir 'PSWUpdateCheck.log'

New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
function Write-Log([string]$Msg) {
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Add-Content -Path $LogPath -Value "[$ts] $Msg" -Encoding UTF8
}

# v1.1: outdated PSWloader.elf. A release older than PSWloader v1.0.5 cannot start the firmware the updater placed on the
# EFI partition, so Windows then runs on the old firmware built into that ELF (restarts, drivers and fan control can
# fail). Warned at every sign-in, also with update checks turned off: the running firmware revision (BIOS version
# "PSWloader v1.1 (r5.26.1)"; no revision = before r5.25.13) is compared with minFirmware from version.json.
function Get-FwRev([string]$s) {   # "r5.26.1" -> 5026001, $null without a revision
    if ($s -match 'r(\d+)\.(\d+)\.(\d+)') { return [int64]$Matches[1] * 1000000 + [int64]$Matches[2] * 1000 + [int64]$Matches[3] }
    return $null
}
try {
    $vjFw = Get-Content -LiteralPath $VerPath -Raw | ConvertFrom-Json
    $bios = [string](Get-CimInstance Win32_BIOS).SMBIOSBIOSVersion
    $need = Get-FwRev ([string]$vjFw.minFirmware)
    if ($need -and $bios -like 'PSWloader*') {
        $have = Get-FwRev $bios
        if ($null -eq $have -or $have -lt $need) {
            Write-Log "Outdated PSWloader.elf/firmware: running '$bios', this installation needs $($vjFw.minFirmware) or newer"
            [void](Show-PswDialog -Bar 'PSWloader' -Head 'PSWloader.elf is outdated' -One 'Close' -Text (
                "This Windows is set up for PSWloader $($vjFw.version) (firmware $($vjFw.minFirmware) or newer), " +
                "but the console was started with an older PSWloader.elf (running: $bios).`n`n" +
                "Restarts, drivers and fan control can fail.`n`n" +
                "Replace PSWloader.elf in your Payload Manager with the one from the $($vjFw.version) release, then start PSWloader again."))
        }
    }
} catch { Write-Log "Firmware check skipped: $($_.Exception.Message)" }

function Parse-SemVer([string]$v) {
    if (-not $v) { return [version]'0.0.0' }
    $clean = ($v.Trim() -replace '^[vV]', '') -replace '[^0-9.].*$', ''
    $parts = @($clean.Split('.') | Where-Object { $_ -ne '' })   # v1.2: always a list (a one-part version like "2" was a string and never got its ".0.0")
    while ($parts.Count -lt 3) { $parts += '0' }
    try { return [version](($parts[0..2]) -join '.') } catch { return [version]'0.0.0' }
}

# 1. Load config (default: autoCheck = true)
$cfg = [pscustomobject]@{
    autoCheck = $true
    repo      = 'mr-Native/PSWloader'
}
if (Test-Path -LiteralPath $ConfigPath) {
    try {
        $loaded = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
        if ($null -ne $loaded.autoCheck) { $cfg.autoCheck = [bool]$loaded.autoCheck }
        if ($loaded.repo) { $cfg.repo = [string]$loaded.repo }
    } catch {
        Write-Log "WARN: could not parse $ConfigPath, using defaults"
    }
}

# v1.2: the switch per user (HKCU\Software\PSWloader AutoUpdateCheck, set by the PSWloader Dashboard and by "Turn off update
# checks") wins over updater.json - the check runs without administrator rights and cannot change that machine-wide file
$userAuto = (Get-ItemProperty -Path 'HKCU:\Software\PSWloader' -Name AutoUpdateCheck -ErrorAction SilentlyContinue).AutoUpdateCheck
if ($null -ne $userAuto) { $cfg.autoCheck = ([int]$userAuto -ne 0) }
if (-not $Force -and -not $cfg.autoCheck) {
    Write-Log "Auto update check is disabled ($(if ($null -ne $userAuto) { 'PSWloader Dashboard / HKCU AutoUpdateCheck = 0' } else { $ConfigPath })) - exiting."
    exit 0
}

# 2. Load installed version
$installedStr = 'v1.0.0'
if (Test-Path -LiteralPath $VerPath) {
    try {
        $vj = Get-Content -LiteralPath $VerPath -Raw | ConvertFrom-Json
        if ($vj.version) { $installedStr = [string]$vj.version }
    } catch {}
}
$installedVer = Parse-SemVer $installedStr

function Compare-PswVersion([string]$A, [string]$B) {   # -1 / 0 / 1, $null when one of them is not a version; a release ranks above its own betas (as in PSWUpdate.ps1)
    $re = '^\s*[vV]?(\d+(?:\.\d+)*)(?:-(\S+))?\s*$'
    if ($A -notmatch $re -or $B -notmatch $re) { return $null }
    $p = foreach ($v in $A, $B) {
        [void]($v -match $re)
        $n = @($Matches[1].Split('.') | ForEach-Object { [int]$_ }); while ($n.Count -lt 3) { $n += 0 }
        , @($n, $Matches[2]) }
    for ($i = 0; $i -lt [Math]::Max($p[0][0].Count, $p[1][0].Count); $i++) {
        $x = if ($i -lt $p[0][0].Count) { $p[0][0][$i] } else { 0 }; $y = if ($i -lt $p[1][0].Count) { $p[1][0][$i] } else { 0 }
        if ($x -ne $y) { return [Math]::Sign($x - $y) } }
    if ($p[0][1] -eq $p[1][1]) { return 0 }
    if (-not $p[0][1]) { return 1 }; if (-not $p[1][1]) { return -1 }
    return [Math]::Sign([string]::CompareOrdinal($p[0][1], $p[1][1]))
}
$UseBeta = [bool]$Beta -or ((Get-ItemProperty -Path 'HKCU:\Software\PSWloader' -Name BetaUpdates -ErrorAction SilentlyContinue).BetaUpdates -eq 1)

# 3. Query GitHub latest release (wait up to 45s for network after boot); with the beta channel on: the newest of all
#    published releases, pre-releases included
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$release = $null
$latestStr = ''
$zipUrl = ''
$zipName = ''

if ($OverrideLatest) {
    $latestStr = $OverrideLatest
} else {
    $apiUrl = if ($UseBeta) { "https://api.github.com/repos/$($cfg.repo)/releases?per_page=30" } else { "https://api.github.com/repos/$($cfg.repo)/releases/latest" }
    for ($try = 1; $try -le 3; $try++) {
        try {
            $release = Invoke-RestMethod -Uri $apiUrl -Headers @{ 'User-Agent' = 'PSWloader-Updater' } -TimeoutSec 12
            if ($UseBeta) {   # newest by version among the published releases
                $best = $null; foreach ($r in @($release | Where-Object { $_.tag_name -and -not $_.draft })) { if (-not $best -or (Compare-PswVersion ([string]$r.tag_name) ([string]$best.tag_name)) -gt 0) { $best = $r } }
                $release = $best
            }
            if ($release -and $release.tag_name) {
                $latestStr = [string]$release.tag_name
                break
            }
        } catch {
            if ($try -lt 3) { Start-Sleep -Seconds 15 }
        }
    }
    if (-not $latestStr) {
        Write-Log "Offline or GitHub API unreachable - exiting silently."
        exit 0
    }
    if ($release.assets) {
        $asset = $release.assets | Where-Object { $_.name -match 'Updater\.zip$' } | Select-Object -First 1
        if ($asset) {
            $zipUrl  = [string]$asset.browser_download_url
            $zipName = [string]$asset.name
        }
    }
}

$latestVer = Parse-SemVer $latestStr
Write-Log "Check complete: installed=$installedStr ($installedVer), latest=$latestStr ($latestVer)"

# newer only: numbers first, a release ranks above its own betas (v1.2.1-beta < v1.2.1); unreadable versions fall back to the numbers
$cmp = Compare-PswVersion $latestStr $installedStr
if (($null -ne $cmp -and $cmp -le 0) -or ($null -eq $cmp -and $latestVer -le $installedVer)) {
    exit 0
}
$isBeta = [bool]($release -and $release.prerelease)

# 4. v1.2: update window in the PSWloader look (Update now / Not now / Turn off update checks)
$ans = Show-PswDialog -Bar 'PSWloader Update' -Head "PSWloader $latestStr$(if ($isBeta) { ' (beta)' }) is available" -One 'Update now' -Two 'Turn off update checks' -Three 'Not now' -Preview $Preview `
  -Text "Your version: $installedStr.$(if ($isBeta) { "`n`nThis is a beta version. Beta versions are experimental: drivers can stop working or your installation can be damaged. Use it at your own risk." })`n`nDownload and install the update now? The updater shows its progress and restarts Windows at the end."
if ($Preview) { exit 0 }
$script:choice = switch ($ans) { 1 { 'Yes' } 2 { 'Disable' } default { 'No' } }
Write-Log "User selected: $($script:choice)"

if ($script:choice -eq 'Disable') {
    try {
        # v1.2: per user (no administrator rights needed); the PSWloader Dashboard shows and changes the same switch
        New-Item -Path 'HKCU:\Software\PSWloader' -Force | Out-Null
        Set-ItemProperty -Path 'HKCU:\Software\PSWloader' -Name AutoUpdateCheck -Value 0 -Type DWord
        Write-Log 'Disabled the automatic update check (HKCU AutoUpdateCheck = 0)'
        [void](Show-PswDialog -Bar 'PSWloader Update' -Head 'Update checks turned off' -One 'Close' `
            -Text "PSWloader no longer looks for updates at sign-in.`n`nTo turn the check back on: PSWloader Dashboard > Updates > Automatic update check.")
    } catch {
        Write-Log "ERROR: failed to turn off the update check: $_"
    }
    exit 0
}

if ($script:choice -ne 'Yes') {
    exit 0
}

# 5. Download & extract the Updater ZIP, then launch PSWUpdater*.exe
if (-not $zipUrl) {
    $relPage = "https://github.com/$($cfg.repo)/releases/tag/$latestStr"
    Write-Log "No Updater.zip asset found; opening release page $relPage"
    Start-Process $relPage
    exit 0
}

$stageDir = Join-Path $env:TEMP "PSWUpdate-$latestStr"
$fileName = if ($zipName) { $zipName } else { "pswloader-$latestStr-Updater.zip" }
$zipFile  = Join-Path $stageDir $fileName
try {
    if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    Write-Log "Downloading $zipUrl -> $zipFile"
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipFile -Headers @{ 'User-Agent' = 'PSWloader-Updater' } -UseBasicParsing
    $extractDir = Join-Path $stageDir 'extracted'
    Expand-Archive -Path $zipFile -DestinationPath $extractDir -Force
    $exe = Get-ChildItem -Path $extractDir -Filter 'PSWUpdater*.exe' -Recurse -File | Select-Object -First 1
    if (-not $exe) { throw 'PSWUpdater executable not found in archive.' }
    Write-Log "Launching $($exe.FullName)"
    Start-Process -FilePath $exe.FullName -WorkingDirectory $exe.DirectoryName -Verb RunAs
} catch {
    Write-Log "ERROR: download/launch failed: $_"
    $relUrl = "https://github.com/$($cfg.repo)/releases/latest"   # "Open releases" opens the page, the window stays until Close
    [void](Show-PswDialog -Bar 'PSWloader Update' -Head 'The update could not be downloaded' -One 'Open releases' -Two 'Close' `
          -Text "The PSWloader $latestStr updater could not be downloaded or started:`n$_`n`nYou can download it from the releases page." -Stay @{ 1 = { Start-Process $relUrl } })
}
