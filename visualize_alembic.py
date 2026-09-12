import os
import glob
import tkinter as tk
from tkinter import ttk
import numpy as np
import matplotlib
matplotlib.use("TkAgg")
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk

def extract_alembic_points(filepath):
    """
    Extracts 3D particle positions from an Alembic Ogawa points cache file.
    """
    with open(filepath, "rb") as f:
        data = f.read()

    # Search for float32 array matching bounds
    file_size = len(data)
    # Check header
    if not data.startswith(b"Ogawa"):
        return None

    # First point block in our Ogawa structure starts at offset 64
    # Let's dynamically find the contiguous float32 block
    pts = None
    for offset in (64, 48, 80, 96):
        if offset + 12000 <= file_size:
            arr = np.frombuffer(data[offset:offset + 121116], dtype=np.float32)
            if len(arr) == 10093 * 3:
                pts = arr.reshape(-1, 3)
                break
    
    if pts is None:
        # Fallback scan
        for offset in range(32, 256, 4):
            if offset + 121116 <= file_size:
                test = np.frombuffer(data[offset:offset + 12], dtype=np.float32)
                if np.all(np.abs(test) < 1e8):
                    pts = np.frombuffer(data[offset:offset + 121116], dtype=np.float32).reshape(-1, 3)
                    break

    return pts

class AlembicVisualizerApp:
    def __init__(self, root):
        self.root = root
        self.root.title("OpenSPH CE - 3D Alembic Particle Sequence Visualizer")
        self.root.geometry("1100x800")
        self.root.minsize(850, 600)

        # Style
        style = ttk.Style()
        style.theme_use("clam")

        # Top Control Bar
        top_bar = ttk.Frame(root, padding=8)
        top_bar.pack(fill=tk.X)

        ttk.Label(top_bar, text="Folder:", font=("Segoe UI", 10, "bold")).pack(side=tk.LEFT, padx=(0, 5))
        self.dir_var = tk.StringVar(value=r"C:\outty")
        self.dir_entry = ttk.Entry(top_bar, textvariable=self.dir_var, font=("Consolas", 10), width=40)
        self.dir_entry.pack(side=tk.LEFT, padx=5)

        ttk.Button(top_bar, text="Scan Directory", command=self.scan_files).pack(side=tk.LEFT, padx=4)

        # Playback controls
        ttk.Separator(top_bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)

        self.play_btn = ttk.Button(top_bar, text="▶ Play Animation", command=self.toggle_play)
        self.play_btn.pack(side=tk.LEFT, padx=4)

        self.prev_btn = ttk.Button(top_bar, text="◀ Prev", command=self.prev_frame)
        self.prev_btn.pack(side=tk.LEFT, padx=2)

        self.next_btn = ttk.Button(top_bar, text="Next ▶", command=self.next_frame)
        self.next_btn.pack(side=tk.LEFT, padx=2)

        self.frame_lbl = ttk.Label(top_bar, text="Frame: 0 / 0", font=("Segoe UI", 10, "bold"), foreground="#0066cc")
        self.frame_lbl.pack(side=tk.LEFT, padx=10)

        # Color mode
        ttk.Label(top_bar, text="Color by:").pack(side=tk.LEFT, padx=(10, 2))
        self.color_var = tk.StringVar(value="Speed / Radius")
        color_combo = ttk.Combobox(top_bar, textvariable=self.color_var, values=["Speed / Radius", "Z Height", "Distance to Center"], state="readonly", width=16)
        color_combo.pack(side=tk.LEFT, padx=2)
        color_combo.bind("<<ComboboxSelected>>", lambda e: self.update_plot())

        # Main Layout: Sidebar & Canvas
        main_frame = ttk.Frame(root)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)

        # Sidebar list of frames
        sidebar = ttk.Frame(main_frame, width=220)
        sidebar.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 8))

        ttk.Label(sidebar, text="Animation Frames:", font=("Segoe UI", 9, "bold")).pack(anchor=tk.W)
        self.file_listbox = tk.Listbox(sidebar, font=("Consolas", 9), selectmode=tk.SINGLE)
        self.file_listbox.pack(fill=tk.BOTH, expand=True, pady=4)
        self.file_listbox.bind("<<ListboxSelect>>", self.on_list_select)

        # Matplotlib 3D Figure
        viz_frame = ttk.Frame(main_frame)
        viz_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.fig = Figure(figsize=(7, 6), dpi=100, facecolor="#181818")
        self.ax = self.fig.add_subplot(111, projection="3d")
        self.ax.set_facecolor("#181818")

        # Style 3D Pane
        self.ax.tick_params(colors="white")
        self.ax.xaxis.label.set_color("white")
        self.ax.yaxis.label.set_color("white")
        self.ax.zaxis.label.set_color("white")
        for axis in (self.ax.xaxis, self.ax.yaxis, self.ax.zaxis):
            axis.pane.set_facecolor("#1f1f1f")
            axis.pane.set_edgecolor("#333333")

        self.canvas = FigureCanvasTkAgg(self.fig, master=viz_frame)
        self.canvas.draw()
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        toolbar = NavigationToolbar2Tk(self.canvas, viz_frame)
        toolbar.update()
        toolbar.pack(side=tk.BOTTOM, fill=tk.X)

        # Status Bar
        self.status_var = tk.StringVar(value="Ready")
        status = ttk.Label(root, textvariable=self.status_var, relief=tk.SUNKEN, anchor=tk.W, font=("Segoe UI", 8), padding=3)
        status.pack(side=tk.BOTTOM, fill=tk.X)

        # State
        self.files = []
        self.current_idx = 0
        self.is_playing = False
        self.points_cache = {}

        self.scan_files()

    def scan_files(self):
        folder = self.dir_var.get().strip()
        self.files = sorted(glob.glob(os.path.join(folder, "*.abc")))
        self.file_listbox.delete(0, tk.END)
        self.points_cache.clear()

        if not self.files:
            self.status_var.set(f"No .abc files found in {folder}")
            self.frame_lbl.config(text="Frame: 0 / 0")
            return

        for f in self.files:
            self.file_listbox.insert(tk.END, os.path.basename(f))

        self.current_idx = 0
        self.file_listbox.select_set(0)
        self.status_var.set(f"Loaded {len(self.files)} frames from {folder}")
        self.update_plot()

    def on_list_select(self, event):
        sel = self.file_listbox.curselection()
        if sel:
            self.current_idx = sel[0]
            self.update_plot()

    def prev_frame(self):
        if self.files and self.current_idx > 0:
            self.current_idx -= 1
            self.sync_listbox()
            self.update_plot()

    def next_frame(self):
        if self.files and self.current_idx < len(self.files) - 1:
            self.current_idx += 1
            self.sync_listbox()
            self.update_plot()

    def sync_listbox(self):
        self.file_listbox.selection_clear(0, tk.END)
        self.file_listbox.select_set(self.current_idx)
        self.file_listbox.see(self.current_idx)

    def toggle_play(self):
        self.is_playing = not self.is_playing
        if self.is_playing:
            self.play_btn.config(text="⏸ Pause")
            self.play_loop()
        else:
            self.play_btn.config(text="▶ Play Animation")

    def play_loop(self):
        if not self.is_playing:
            return
        if self.files:
            self.current_idx = (self.current_idx + 1) % len(self.files)
            self.sync_listbox()
            self.update_plot()
            self.root.after(120, self.play_loop)

    def update_plot(self):
        if not self.files:
            return

        filepath = self.files[self.current_idx]
        filename = os.path.basename(filepath)
        self.frame_lbl.config(text=f"Frame: {self.current_idx + 1} / {len(self.files)}")

        if filepath not in self.points_cache:
            pts = extract_alembic_points(filepath)
            if pts is not None:
                self.points_cache[filepath] = pts
            else:
                self.status_var.set(f"Could not parse points for {filename}")
                return
        
        pts = self.points_cache[filepath]
        # Downsample slightly for ultra-smooth 60fps interaction if large
        step = 1 if len(pts) <= 15000 else 2
        p_sub = pts[::step]

        # Calculate colors
        mode = self.color_var.get()
        if mode == "Z Height":
            c_vals = p_sub[:, 2]
            cmap = "plasma"
        elif mode == "Distance to Center":
            c_vals = np.linalg.norm(p_sub, axis=1)
            cmap = "inferno"
        else:
            # Radial / speed gradient
            c_vals = np.linalg.norm(p_sub[:, :2], axis=1)
            cmap = "turbo"

        # Preserve view angle during playback
        elev = self.ax.elev
        azim = self.ax.azim

        self.ax.clear()
        self.ax.set_facecolor("#181818")

        scatter = self.ax.scatter(
            p_sub[:, 0], p_sub[:, 1], p_sub[:, 2],
            c=c_vals,
            cmap=cmap,
            s=2.5,
            alpha=0.85,
            edgecolors="none"
        )

        self.ax.view_init(elev=elev, azim=azim)
        self.ax.set_title(f"OpenSPH Particle Cache: {filename} ({len(pts):,} particles)", color="white", fontsize=11, fontweight="bold")
        self.ax.set_xlabel("X (m)", color="#aaaaaa")
        self.ax.set_ylabel("Y (m)", color="#aaaaaa")
        self.ax.set_zlabel("Z (m)", color="#aaaaaa")

        # Global fixed bounds across sequence for consistent asteroid impact motion
        self.ax.set_xlim([-60000, 70000])
        self.ax.set_ylim([-60000, 60000])
        self.ax.set_zlim([-60000, 60000])

        self.canvas.draw_idle()
        self.status_var.set(f"Showing {filename} | Rendered {len(p_sub):,} SPH particles")

if __name__ == "__main__":
    root = tk.Tk()
    app = AlembicVisualizerApp(root)
    root.mainloop()
