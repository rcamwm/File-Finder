# File-Finder
Finds files or specific text in files in subdirectories

## How to use
After compiling and running program, can use the following commands (omit angle brackets):

    find <filename>
    
Searches for a file named **filename** in current directory.

    find <”text”>
    
Searches for a file that contains "**text**" in current directory.

    <command> -s

Flag that can be used with any *find* command. Extends search to include all subdirectories.

    <command> -f:<extension>
    
Flag that can be used with text-searching *find* command. Limits search to only files that end with **.extension**.

    list
    
Lists all currently running search processes and what they're searching for.

    kill <num>
    
Kills a certain process. **num** can be found with *list* command.

    quit
  
Quits program and ends all processes.
