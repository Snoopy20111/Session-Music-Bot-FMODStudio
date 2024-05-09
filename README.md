# Session Music Bot

Built with VS2022 for Windows, x86 and x64.

Built with FMOD Engine API 2.02.16. May work with later versions as the API is really stable but do so at your own risk. Also using D++ (version unknown, this was built from their most current VS Template).

When building locally, be sure to do the following:
- Make sure the paths in the Post-Build step are correct for where you installed the FMOD API. I'm not a master of Visual Studio Macros yet so I haven't found an easy way for other users to define them.
- Ensure you've [created a Discord Bot Token](https://dpp.dev/creating-a-bot-application.html) and set everything up on Discord's end.
- Ensure you've renamed MyBot\token.config.example to MyBot\token.config and replaced _all_ the text in it with your Bot Token! The program, as it is now, will read that in at startup and use it when initializing your Bot.