// Offline recognition with the pinned TiDB parser. No database client/session
// is constructed. The test_driver import supplies AST literal implementations.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"runtime/debug"

	"github.com/pingcap/tidb/pkg/parser"
	"github.com/pingcap/tidb/pkg/parser/mysql"
	_ "github.com/pingcap/tidb/pkg/parser/test_driver"
)

const moduleName = "github.com/pingcap/tidb/pkg/parser"
const moduleVersion = "v0.0.0-20261126093401-a3ad25ec1c13"
const revision = "a3ad25ec1c13667ade6e531ea25d8549df49e319"

type request struct {
	Index int    `json:"index"`
	SQL   string `json:"sql"`
	Mode  string `json:"mode"`
}

type response struct {
	Index        int      `json:"index"`
	Accepted     bool     `json:"accepted"`
	Statements   int      `json:"statements"`
	Error        string   `json:"error,omitempty"`
	Warnings     []string `json:"warnings,omitempty"`
	HarnessError bool     `json:"harness_error,omitempty"`
}

func bounded(message string) string {
	if len(message) > 2048 {
		return message[:2048]
	}
	return message
}

func main() {
	info, ok := debug.ReadBuildInfo()
	matched := false
	if ok {
		for _, dependency := range info.Deps {
			if dependency.Path == moduleName && dependency.Version == moduleVersion && dependency.Replace == nil {
				matched = true
			}
		}
	}
	if !matched {
		fmt.Fprintln(os.Stderr, "TiDB parser module identity does not match the pinned reference")
		os.Exit(2)
	}
	if len(os.Args) == 2 && os.Args[1] == "--version" {
		_ = json.NewEncoder(os.Stdout).Encode(map[string]string{
			"module": moduleName, "version": moduleVersion, "revision": revision,
		})
		return
	}
	if len(os.Args) != 1 {
		fmt.Fprintln(os.Stderr, "usage: tidb-native-probe [--version]; JSON requests on stdin")
		os.Exit(2)
	}
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 4096), 2*1024*1024)
	output := bufio.NewWriter(os.Stdout)
	encoder := json.NewEncoder(output)
	engine := parser.New()
	engine.EnableWindowFunc(true)
	for scanner.Scan() {
		var input request
		result := response{}
		if err := json.Unmarshal(scanner.Bytes(), &input); err != nil {
			result.HarnessError, result.Error = true, "invalid request JSON"
		} else {
			result.Index = input.Index
			mode, err := mysql.GetSQLMode(input.Mode)
			if len(input.SQL) > 262144 || len(input.Mode) > 1024 {
				result.HarnessError, result.Error = true, "request exceeds probe limits"
			} else if err != nil {
				result.HarnessError, result.Error = true, bounded(err.Error())
			} else {
				engine.SetSQLMode(mode)
				nodes, warnings, err := engine.ParseSQL(input.SQL)
				result.Accepted, result.Statements = err == nil, len(nodes)
				if err != nil {
					result.Error = bounded(err.Error())
				}
				for index, warning := range warnings {
					if index == 16 {
						break
					}
					result.Warnings = append(result.Warnings, bounded(warning.Error()))
				}
			}
		}
		if err := encoder.Encode(result); err != nil {
			os.Exit(2)
		}
		if err := output.Flush(); err != nil {
			os.Exit(2)
		}
	}
	if err := scanner.Err(); err != nil {
		fmt.Fprintln(os.Stderr, bounded(err.Error()))
		os.Exit(2)
	}
}
