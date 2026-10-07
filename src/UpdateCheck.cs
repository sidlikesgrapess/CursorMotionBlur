using System;
using System.IO;
using System.Net;
using System.Text.RegularExpressions;
using System.Threading;

namespace CursorMotionBlur
{
    /// <summary>
    /// Asks GitHub for the latest release and compares it with this version. It only runs when the user clicks
    /// "Check for updates"; the app never contacts the internet on its own.
    /// </summary>
    static class UpdateCheck
    {
        const string Repo = "sidlikesgrapess/CursorMotionBlur";

        /// <summary>Calls done(message, url) on a background thread; url is set only when a newer version exists.</summary>
        public static void Run(Action<string, string> done)
        {
            ThreadPool.QueueUserWorkItem(delegate
            {
                try
                {
                    ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
                    var req = (HttpWebRequest)WebRequest.Create("https://api.github.com/repos/" + Repo + "/releases/latest");
                    req.UserAgent = "CursorMotionBlur/" + AppInfo.Version;   // GitHub's API refuses requests without a user agent
                    req.Timeout = 8000;
                    string json;
                    using (var resp = req.GetResponse())
                    using (var reader = new StreamReader(resp.GetResponseStream())) json = reader.ReadToEnd();

                    var tag = Regex.Match(json, "\"tag_name\"\\s*:\\s*\"v?([^\"]+)\"");
                    Version latest, current;
                    if (!tag.Success || !Version.TryParse(tag.Groups[1].Value, out latest) || !Version.TryParse(AppInfo.Version, out current))
                    {
                        done("Couldn't read the latest version", null);
                        return;
                    }
                    if (latest > current) done("Version " + latest + " is available", "https://github.com/" + Repo + "/releases/tag/v" + latest);
                    else done("You're up to date (v" + AppInfo.Version + ")", null);
                }
                catch (Exception)
                {
                    done("Couldn't check (no connection?)", null);
                }
            });
        }
    }
}
