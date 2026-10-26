import os
import re
import shutil
import subprocess
import questionary

TOKEN_FILE = ".github_token"
GITHUB_USERNAME = "nikoniko78"
GITHUB_REPO = "PaperWeatherMain"

def find_downloads_folder():
    """Finds the user's Downloads directory across operating systems."""
    home = os.path.expanduser("~")
    downloads = os.path.join(home, "Downloads")
    if os.path.exists(downloads):
        return downloads
    return home

def get_latest_main_cpp(downloads_dir):
    """
    Scans the Downloads folder for main.cpp and variants like main(1).cpp, 
    returning the path of the file with the highest version number.
    """
    if not os.path.exists(downloads_dir):
        return None

    pattern = re.compile(r"^main(?:\s*\((\d+)\))?\.cpp$", re.IGNORECASE)
    
    highest_num = -1
    best_file = None

    for filename in os.listdir(downloads_dir):
        match = pattern.match(filename)
        if match:
            num_str = match.group(1)
            num = int(num_str) if num_str is not None else 0
            
            if num > highest_num:
                highest_num = num
                best_file = os.path.join(downloads_dir, filename)

    return best_file

def find_platformio_projects(root_dir):
    """Recursively scan for platformio.ini files to locate projects."""
    projects = []
    for dirpath, dirnames, filenames in os.walk(root_dir):
        dirnames[:] = [d for d in dirnames if not d.startswith('.')]
        if "platformio.ini" in filenames:
            projects.append(dirpath)
    return projects

def get_auto_version_recommendation(project_path):
    """
    Scans the updates folder in the project to suggest an auto-incremented version number,
    supporting optional prefixes ('n', 'dn', 'dy').
    """
    updates_dir = os.path.join(project_path, "updates")
    if not os.path.exists(updates_dir):
        return "1.0.0"

    version_pattern = re.compile(r"(?:n|d[ny])?([0-9]+\.[0-9]+(?:\.[0-9]+)?)")
    highest_v = [0, 0, 0]

    for dirname in os.listdir(updates_dir):
        match = version_pattern.search(dirname)
        if match:
            try:
                parts = [int(p) for p in match.group(1).split(".")]
                while len(parts) < 3:
                    parts.append(0)
                
                if parts > highest_v:
                    highest_v = parts
            except ValueError:
                continue

    if highest_v != [0, 0, 0]:
        highest_v[2] += 1
        return f"{highest_v[0]}.{highest_v[1]}.{highest_v[2]}"
    
    return "1.0.0"

def run_pio_command(project_path, action_mode):
    """Execute the PlatformIO build or build+flash command in the selected project directory."""
    if action_mode == "Build & Flash":
        cmd = ["pio", "run", "-t", "upload"]
    else:
        cmd = ["pio", "run"]

    print(f"\nRunning: {' '.join(cmd)}")
    print(f"Working Directory: {project_path}\n" + "-"*40)

    try:
        subprocess.run(cmd, cwd=project_path, check=True)
        print("\n" + "-"*40 + f"\nPlatformIO {action_mode.lower()} completed successfully.")
        return True
    except subprocess.CalledProcessError as e:
        print("\n" + "-"*40 + f"\nTask failed with exit code {e.returncode}.")
        return False
    except FileNotFoundError:
        print("\nError: PlatformIO ('pio') CLI tool not found. Make sure PlatformIO Core is installed and added to your system PATH.")
        return False

def find_firmware_binary(project_path):
    """Locates the compiled firmware.bin inside the .pio build directory."""
    pio_dir = os.path.join(project_path, ".pio", "build")
    if not os.path.exists(pio_dir):
        return None

    for env_name in os.listdir(pio_dir):
        env_path = os.path.join(pio_dir, env_name)
        if os.path.isdir(env_path):
            firmware_path = os.path.join(env_path, "firmware.bin")
            if os.path.exists(firmware_path):
                return firmware_path
    return None

def get_or_request_token(project_path):
    """Retrieves saved token or prompts the user for it and saves it."""
    token_path = os.path.join(project_path, TOKEN_FILE)
    
    if os.path.exists(token_path):
        with open(token_path, "r") as f:
            token = f.read().strip()
            if token:
                return token

    token = questionary.password(
        "Enter your GitHub Personal Access Token (PAT):"
    ).ask()

    if token:
        with open(token_path, "w") as f:
            f.write(token.strip())
        print(f"Token saved securely to {TOKEN_FILE}")
        
    return token

def post_to_github(project_path, version):
    """
    Initializes git if needed, stages the updates folder additively, 
    commits, and pushes without wiping history.
    """
    print("\nPreparing to post update to GitHub...")
    
    token = get_or_request_token(project_path)
    if not token:
        print("Error: No GitHub token provided. Aborting push.")
        return

    repo_url = f"https://{GITHUB_USERNAME}:{token}@github.com/{GITHUB_USERNAME}/{GITHUB_REPO}.git"
    git_dir = os.path.join(project_path, ".git")

    try:
        if not os.path.exists(git_dir):
            subprocess.run(["git", "init"], cwd=project_path, check=True)
            subprocess.run(["git", "checkout", "-b", "main"], cwd=project_path, check=True)

        result = subprocess.run(["git", "remote", "get-url", "origin"], cwd=project_path, capture_output=True, text=True)
        if result.returncode != 0:
            subprocess.run(["git", "remote", "add", "origin", repo_url], cwd=project_path, check=True)
        else:
            subprocess.run(["git", "remote", "set-url", "origin", repo_url], cwd=project_path, check=True)

        # Pull any existing remote changes first to avoid fast-forward rejection
        subprocess.run(["git", "pull", "origin", "main", "--rebase"], cwd=project_path, check=False)

        # Stage ONLY the updates folder additively
        subprocess.run(["git", "add", "-f", "updates/"], cwd=project_path, check=True)
        
        commit_msg = f"Add update version {version}"
        subprocess.run(["git", "commit", "-m", commit_msg], cwd=project_path, check=False)
        
        # Standard push (additive, preserves other version folders)
        subprocess.run(["git", "push", "origin", "main"], cwd=project_path, check=True)
        print("Successfully pushed update to GitHub repository.")
    except subprocess.CalledProcessError as e:
        print(f"Git operation failed: {e}. Make sure git is initialized and your token has repo permissions.")

def main():
    workspace_dir = os.getcwd()
    
    print("Scanning workspace for PlatformIO projects...")
    projects = find_platformio_projects(workspace_dir)

    if not projects:
        print("No PlatformIO projects (platformio.ini) found in this directory or its subdirectories.")
        return

    # TUI: Project Selection Menu
    selected_project = questionary.select(
        "Select a PlatformIO project to manage:",
        choices=projects
    ).ask()

    if not selected_project:
        print("Operation cancelled.")
        return

    # Find latest main.cpp in Downloads
    downloads_dir = find_downloads_folder()
    latest_main = get_latest_main_cpp(downloads_dir)

    if not latest_main:
        print(f"No main.cpp files found in Downloads folder: {downloads_dir}")
        return

    print(f"Found latest main file in Downloads: {latest_main}")

    # Ensure project src directory exists
    src_dir = os.path.join(selected_project, "src")
    os.makedirs(src_dir, exist_ok=True)

    dest_main = os.path.join(src_dir, "main.cpp")
    
    # Move and overwrite
    shutil.copy2(latest_main, dest_main)
    print(f"Copied and overwritten {latest_main} to {dest_main}")

    # Build / Flash Mode Selection Menu
    action_mode = questionary.select(
        "Select PlatformIO action:",
        choices=["Build only", "Build & Flash"]
    ).ask()

    if not action_mode:
        print("Operation cancelled.")
        return

    # Execute PlatformIO Action
    success = run_pio_command(selected_project, action_mode)
    if not success:
        return

    # Single Prompt for Binary Construction & GitHub Posting
    construct_and_post = questionary.confirm(
        "Would you like to create a versioned binary and post it to GitHub?"
    ).ask()

    if construct_and_post:
        recommended_version = get_auto_version_recommendation(selected_project)
        
        base_version = questionary.text(
            f"What should the base version number be? (Recommended: {recommended_version}):",
            default=str(recommended_version)
        ).ask()

        if not base_version:
            base_version = str(recommended_version)

        # Nightly vs Description Logic (Mutually Exclusive)
        is_nightly = questionary.confirm(
            "Would you like to make this a Nightly Update?"
        ).ask()

        info_text = None
        if is_nightly:
            version_input = f"n{base_version}" if not base_version.startswith("n") else base_version
        else:
            add_description = questionary.confirm(
                "Would you like to add a description?"
            ).ask()

            if add_description:
                allow_install = questionary.confirm(
                    "Should this update be installable on the device?"
                ).ask()
                
                # dy = allow install, dn = block install
                prefix = "dy" if allow_install else "dn"
                version_input = f"{prefix}{base_version}"
                
                info_text = questionary.text("Enter update description text for info.txt:").ask()
            else:
                version_input = base_version

        firmware_bin = find_firmware_binary(selected_project)
        if firmware_bin and os.path.exists(firmware_bin):
            updates_dir = os.path.join(selected_project, "updates", version_input)
            os.makedirs(updates_dir, exist_ok=True)

            # Copy main.bin
            target_bin_path = os.path.join(updates_dir, "main.bin")
            shutil.copy2(firmware_bin, target_bin_path)
            print(f"Successfully created binary at: {target_bin_path}")

            # Create info.txt if description was provided
            if info_text:
                target_info_path = os.path.join(updates_dir, "info.txt")
                with open(target_info_path, "w") as f:
                    f.write(info_text.strip())
                print(f"Successfully created info.txt at: {target_info_path}")

            # Post additively to GitHub
            post_to_github(selected_project, version_input)
        else:
            print("Could not locate compiled firmware.bin in the build artifacts.")

if __name__ == "__main__":
    main()
