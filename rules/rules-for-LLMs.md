# Rules
This is an industrialization project, and you should obey rules below:
- Keep workspace clean. Temporary tests or thirdparty data (like reference projects) are not allowed to be committed into git. The temp/ dir is specially used for temp data.
- Note system. Long-term issues or tips are valued so there is a designed note system.
- Check first. Code wouldn't run well in the first time in most cases, but you can check first. Check all the changed code before running tests, and attemp to find critical bugs.

More details about Note System:
- The notes are numbered as <NUM[:TIPS]>, and they are recorded in notes/ dir. notes/index.md records all the notes with a description, a state, and the reference. For example:
```markdown
The process of project: DESCRIPTION
<1>(SOLVED/UNRESOLVED/TIPS): a brief description
...
<NUM>(...): ...
```
- All the unresolved notes are written in notes/current.md, and the format is like:
```markdown
# <NUM>: the brief description
SOLVED/UNRESOLVED/TIPS
The note description and experience.
```
- Notes files are bound with git tag, when a git tag is set, you should rename notes/current.md to <tag-name>.md and create a new current file.
- You should record important things in notes and maintain the note system when modifying. Number sequentially increases. Any change must be synced with index.md.

# Standard Project Work Flow
A project is devided into steps.
- **1. Setup**: You should follow user's instructions, setup git repo and build system. You should clearify the environment(python and C++ compiler version and so on) and record it to temp/environment.md. Some system environment may disallow modifying so you should setup virtual environment in project or for tool use in temp/ dir.
- **2. Write internal docs**: This project is doc and test driven. Write LLM-readable docs in doc-internal/ dir, clearfying internal implementations and external interfaces.
- **3. Define and write tests**: Complete external interfaces first, and write detailed unit tests for correctness verification.
- **4. Implementation**: Write code to complete the project. You may not capable to complete in one round of chat, split and do serveral times. Finish the code and ensure unit test pass first.
- **5. Doc and perfecting**: Write docs and optimize the project.
- **6. Long-term maintainance**: Continuing fix bugs, add new functions and optimizations.

Maintain rules/custom-rule-of-LLMs.md for customed rules.
Read rules/user.md for user-customed rules and rules/goal.md for project description.
