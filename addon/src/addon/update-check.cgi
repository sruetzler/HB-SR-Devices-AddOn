#!/bin/tclsh
# GitHub's latest release excludes drafts and prereleases. No Tcllib required.
set checkURL "https://api.github.com/repos/sruetzler/HB-SR-Devices-AddOn/releases/latest"
set releaseBase "https://github.com/sruetzler/HB-SR-Devices-AddOn/releases/download"
set cmd ""
if {[info exists env(QUERY_STRING)]} {
  foreach pair [split $env(QUERY_STRING) &] {
    if {[string compare $pair "cmd=download"] == 0} {
      set cmd "download"
    }
  }
}

set newversion "n/a"
set downloadURL ""
# Accept only numeric release tags and the matching package URL. In particular,
# query parameters must never override the trusted API or download addresses.
catch {
  # BusyBox wget does not support GNU wget's -t option. Forward diagnostics
  # to stderr: Tcl otherwise treats even successful commands with stderr
  # output as failures. HTTP/process errors still make exec fail.
  set release [exec /usr/bin/wget -qO- -T 15 $checkURL 2>@ stderr]
  if {[regexp {"tag_name"[[:space:]]*:[[:space:]]*"(v?([0-9]+(?:\.[0-9]+)+))"} $release unused tag version]} {
    set expectedURL "$releaseBase/$tag/hb-sr-devices-addon.tgz"
    # Tcl 8.2 has no regexp -all/-inline; consume one URL at a time.
    set remaining $release
    set assetPattern {"browser_download_url"[[:space:]]*:[[:space:]]*"([^"\\]+)"}
    while {[regexp -indices $assetPattern $remaining matchRange urlRange]} {
      set url [string range $remaining [lindex $urlRange 0] [lindex $urlRange 1]]
      set remaining [string range $remaining [expr {[lindex $matchRange 1] + 1}] end]
      if {[string compare $url $expectedURL] == 0} {
        set downloadURL $url
        set newversion $version
        break
      }
    }
  }
}

if {[string compare $cmd "download"] == 0} {
  if {[string length $downloadURL] > 0} {
    puts -nonewline "Status: 302 Found\r\nLocation: $downloadURL\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
  } else {
    puts -nonewline "Status: 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
    puts "Kein herunterladbares GitHub-Release verfuegbar."
  }
} else {
  puts -nonewline "Content-Type: text/plain; charset=utf-8\r\n\r\n"
  puts $newversion
}
