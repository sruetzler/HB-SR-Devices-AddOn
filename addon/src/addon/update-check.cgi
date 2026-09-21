#!/bin/tclsh
# GitHub's latest release excludes drafts and prereleases. No Tcllib required.
set checkURL "https://api.github.com/repos/sruetzler/HB-SR-Devices-AddOn/releases/latest"
set releaseBase "https://github.com/sruetzler/HB-SR-Devices-AddOn/releases/download"
set cmd ""
if {[info exists env(QUERY_STRING)]} {
  foreach pair [split $env(QUERY_STRING) &] {
    if {$pair eq "cmd=download"} {
      set cmd "download"
    }
  }
}

set newversion "n/a"
set downloadURL ""
# Accept only numeric release tags and the matching package URL. In particular,
# query parameters must never override the trusted API or download addresses.
catch {
  set release [exec /usr/bin/wget -qO- -T 15 -t 1 $checkURL]
  if {[regexp {"tag_name"[[:space:]]*:[[:space:]]*"(v?([0-9]+(?:\.[0-9]+)+))"} $release unused tag version]} {
    set expectedURL "$releaseBase/$tag/hb-sr-devices-addon.tgz"
    foreach {unused url} [regexp -all -inline {"browser_download_url"[[:space:]]*:[[:space:]]*"([^"\\]+)"} $release] {
      if {$url eq $expectedURL} {
        set downloadURL $url
        set newversion $version
        break
      }
    }
  }
}

if {$cmd eq "download"} {
  if {$downloadURL ne ""} {
    puts -nonewline "Status: 302 Found\r\nLocation: $downloadURL\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
  } else {
    puts -nonewline "Status: 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
    puts "Kein herunterladbares GitHub-Release verfuegbar."
  }
} else {
  puts -nonewline "Content-Type: text/plain; charset=utf-8\r\n\r\n"
  puts $newversion
}
