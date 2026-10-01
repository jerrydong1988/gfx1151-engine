"""Long context with a sustained code-generation output; no business-quality oracle.

Uses the sibling long_context client, including its offline default, --dry-run,
--selftest and explicit --run modes. Set TOK_DIR to an existing tokenizer and
GDEC_TEST_OUT to the desired live results directory. No engine is started here.
"""
import sys

sys.dont_write_bytecode = True
import long_context as harness

harness.SUFFIX=(
    '\n[DATA END]\n请编写完整的Python合同台账核对程序。不要计算以上合成文本的总额。'
    '程序应使用Decimal统一元和万元，保留来源行号，规范合同编号，检测完全重复和金额冲突，'
    '按部门和年度汇总，并将统计与异常清单输出JSON。输入是字典列表。'
    '使用类型标注、独立函数和清楚的中文注释，不使用第三方库。'
    '给出完整实现，再提供覆盖单位、重复、冲突、缺失值、负金额和跨年的unittest测试。'
    '请直接输出代码，不省略函数体，不使用占位符。'
    '<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
)

if __name__=='__main__':
    harness.main()
