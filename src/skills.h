#ifndef CCLAW_SKILLS_H
#define CCLAW_SKILLS_H

#include <stddef.h>
#include <sqlite3.h>

/* Agent Skills (agentskills.io convention, shared by pi / OpenClaw / Claude
 * Code): a skill is a directory containing SKILL.md with YAML frontmatter
 * (`name`, `description`), or a bare .md file with the same frontmatter.
 * Progressive disclosure: only an <available_skills> index (name, description,
 * location) is injected into the system prompt; the model reads the body with
 * file_read when a task matches — so skill bodies must live on the filesystem
 * at paths the file-tool sandbox can read (skills_dirs are mounted read-only
 * into file-tier children).
 *
 * Discovery: every dir in the `skills_dirs` config key (JSON array, `~`
 * expanded), then the per-agent dir agents/<name>/skills/. First name wins on
 * collision. */

/* Resolved discovery dirs for an agent: skills_dirs entries that exist on
 * disk plus the per-agent skills dir. Heap array of heap strings; caller
 * frees via skills_dirs_free. agents_dir/agent may be NULL (skipped). */
char **skills_dirs_resolve(sqlite3 *db, const char *agents_dir,
                           const char *agent, size_t *count);
void skills_dirs_free(char **dirs, size_t count);

/* Build the <available_skills> prompt block (with a one-line usage hint)
 * from every skill discovered under the agent's dirs. Returns malloc'd
 * string, or NULL if no skills were found. */
char *skills_index_build(sqlite3 *db, const char *agents_dir, const char *agent);

/* Parse `name`/`description` from a SKILL.md-style YAML frontmatter block.
 * Reads at most the head of the file. Returns 0 and sets *name_out (may be
 * NULL if absent — caller applies its fallback) and *desc_out (heap strings)
 * on success; -1 if the file has no usable frontmatter or no description.
 * Exposed for tests. */
int skill_frontmatter_parse(const char *path, char **name_out, char **desc_out);

#endif
