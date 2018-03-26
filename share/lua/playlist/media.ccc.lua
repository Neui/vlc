--[[
Resolve media.ccc.de video URLs

 $Id$
 Copyright © 2018 Neui

 Author: Neui

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
--]]

sxml = require("simplexml")
require("common")

function trim(s)
    if not s then return s end
    return string.match( s, "^%s*(.+)%s*$" )
end

function probe()
    return ( vlc.access == "http" or vlc.access == "https" )
            and
           ( string.match( vlc.path, "media%.ccc%.de/v/.+" )
            or string.match( vlc.path, "media%.ccc%.de/v/.+/oembed$" ) )
end

function parse()
    if string.find( vlc.path, "/oembed" ) then
        return {
            {
                path=vlc.access.."://"..string.sub( vlc.path, 1, -#"/oembed" -  1)
            }
        }
    end

    local page = ""
    while true do
        line = vlc.readline()
        if not line then break end
        page = page .. line
    end

    local title = string.match( page, "<h1>(.-)</h1>" )
    title = trim( title )

    local subtitle = string.match( page, "<h2>(.-)</h2>" )
    subtitle = trim( subtitle )

    local poster = string.match( page, "poster='(.-)'" )

    local date = string.match( page, "<li><span .- title='event date'></span>(.-)</li>" )
    if not date then
        date = string.match( page, "<li><span .- title='release date'></span>(.-)</li>" )
    end
    if not date then
        date = string.match( page, "<li><span .- title='event and release date'></span>(.-)</li>" )
    end

    local author_context = string.match( page, "<p class='persons'>(.-)<div class='player video'>" )
    local authors = {}
    for match in string.gmatch( author_context, "<a href='.-'>(.-)</a>" ) do
        authors[#authors + 1] = trim( match )
    end

    local sources = {}
    for match in string.gmatch( page, "<source (.-)>" ) do
        local video = {}
        video.lang = string.match( match, "data%-lang='(.-)'" )
        video.qualitystr = string.match( match, "data%-quality='(.-)'" )
        video.quality = ({ high=720, low=576 })[video.qualitystr]
        video.src = string.match( match, "src='(.-)'" )
        sources[#sources + 1] = video
    end

    local preferred_res = vlc.var.inherit(nil, "preferred-resolution") or 720
    local perfect_sources = {}
    for k, v in pairs(sources) do
        if v.quality <= preferred_res then
            perfect_sources[#perfect_sources + 1] = v
        end
    end
    if #perfect_sources == 0 then perfect_sources[1] = sources[1] end

    local video = perfect_sources[1]
    return {
            {
                path=video.src;
                name=title;
                title=title;
                description=subtitle;
                artist=table.concat(authors, ", ");
                date=date;
                language=video.lang;
                arturl=poster;
        }
    }

end
