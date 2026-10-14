// Shared helper: expand "@path" script arguments into the non-blank, non-# lines of that file.
// Needed because analyzeHeadless.bat runs through cmd.exe, which mangles regex characters like | ^ ( ).
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class ArgFiles {
	public static String[] expand(String[] args) throws IOException {
		List<String> out = new ArrayList<>();
		for (String a : args) {
			if (!a.startsWith("@")) {
				out.add(a);
				continue;
			}
			for (String line : Files.readAllLines(Path.of(a.substring(1)), StandardCharsets.UTF_8)) {
				String t = line.strip();
				if (!t.isEmpty() && !t.startsWith("#")) {
					out.add(t);
				}
			}
		}
		return out.toArray(new String[0]);
	}
}
