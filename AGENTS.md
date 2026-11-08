# Agent instructions

## Project navigation

- Read [docs/project-structure.md](docs/project-structure.md) for the repository
  map, runtime flow, generated-code boundaries, and task-specific starting points.

## Protected documentation

- Never edit any `README.md` or `CONTRIBUTING.md` file unless the user
  specifically requests changes to that file.
- When a change is explicitly requested, modify only the sections or content
  the user asked to change. Do not rewrite, reformat, reorganize, or otherwise
  modify unrelated parts of the file.
- General implementation requests and instructions to update related
  documentation do not authorize changes to these protected files.

## Additional rules

- All of the text visible to the user must be human-written. If additional text
  that has not been supplied is required and the user is unavailable, leave a
  placeholder and leave a notification of the placeholder placement when finished.
- All of platform-specific code must be contained in respective platform folders.
  If some functionality that needs to be platform-specific is not provided by
  the portability interface, modify the interface's structure while adhering to
  the platform isolation rule.
- Generate relevant documentation as you progress, ensuring small models can
  still operate without confusion.
